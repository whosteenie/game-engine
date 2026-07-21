#include "engine/rendering/post/BloomTonemapPass.h"

#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"

#include <algorithm>

namespace
{
    void DrawBloomBlurPass(
        const PostProcessContext& context,
        Shader& bloomBlurShader,
        PostProcessTarget& target,
        const std::uintptr_t inputSrv,
        const float dirX,
        const float dirY,
        const float blurRadius,
        const int bloomWidth,
        const int bloomHeight,
        const float clearColor[4])
    {
        bloomBlurShader.Use(false, false);
        bloomBlurShader.SetFloat("uDirectionX", dirX);
        bloomBlurShader.SetFloat("uDirectionY", dirY);
        bloomBlurShader.SetFloat("uBlurRadius", blurRadius);
        bloomBlurShader.BindTextureSlot(0, inputSrv);
        context.draw.DrawFullscreenToTarget(
            bloomBlurShader, target, bloomWidth, bloomHeight, clearColor);
    }
}

bool BloomTonemapPass::ExecuteRenderResBloom(
    const PostProcessContext& context,
    const RenderResBloomInputs& inputs,
    RenderResBloomOutputs& outputs)
{
    if (inputs.hdrColorSrv == 0 || inputs.bloomExtractShader == nullptr
        || inputs.bloomBlurShader == nullptr || inputs.bloomExtractTarget == nullptr
        || inputs.bloomBlurTarget == nullptr || inputs.bloomBlur2Target == nullptr)
    {
        return false;
    }

    const int renderWidth = context.renderWidth;
    const int renderHeight = context.renderHeight;
    if (renderWidth <= 0 || renderHeight <= 0)
    {
        return false;
    }

    SceneRenderTrace::Section bloomSection("bloom");
    SceneRenderTrace::Scope bloomExtractScope("bloom extract");
    const int bloomWidth = std::max(1, renderWidth / 2);
    const int bloomHeight = std::max(1, renderHeight / 2);
    const glm::vec2 bloomTexelSize(
        1.0f / static_cast<float>(bloomWidth),
        1.0f / static_cast<float>(bloomHeight));
    const float bloomClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    inputs.bloomExtractShader->SetInt("uHdrColor", 0);
    inputs.bloomExtractShader->SetFloat("uThreshold", inputs.bloomThreshold);
    inputs.bloomExtractShader->SetFloat("uSoftKnee", inputs.bloomSoftKnee);
    inputs.bloomExtractShader->SetFloat("uExposure", inputs.exposure);
    inputs.bloomExtractShader->SetFloat("uFullTexelSizeX", inputs.fullTexelSize.x);
    inputs.bloomExtractShader->SetFloat("uFullTexelSizeY", inputs.fullTexelSize.y);
    inputs.bloomExtractShader->SetFloat(
        "uUseMaterialGbuffer", inputs.useMaterialGbuffer ? 1.0f : 0.0f);
    inputs.bloomExtractShader->BindTextureSlot(0, inputs.hdrColorSrv);
    if (inputs.useMaterialGbuffer)
    {
        inputs.bloomExtractShader->BindTextureSlot(1, inputs.material0Srv);
        inputs.bloomExtractShader->BindTextureSlot(2, inputs.material1Srv);
    }
    context.draw.DrawFullscreenToTarget(
        *inputs.bloomExtractShader,
        *inputs.bloomExtractTarget,
        bloomWidth,
        bloomHeight,
        bloomClear);
    bloomExtractScope.Success();

    SceneRenderTrace::Scope bloomBlurScope("bloom blur");
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlurTarget,
        inputs.bloomExtractTarget->srvCpuHandle,
        bloomTexelSize.x,
        0.0f,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlur2Target,
        inputs.bloomBlurTarget->srvCpuHandle,
        0.0f,
        bloomTexelSize.y,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlurTarget,
        inputs.bloomBlur2Target->srvCpuHandle,
        bloomTexelSize.x,
        0.0f,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlur2Target,
        inputs.bloomBlurTarget->srvCpuHandle,
        0.0f,
        bloomTexelSize.y,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    bloomBlurScope.Success();

    outputs.bloomSrv = inputs.bloomBlur2Target->srvCpuHandle;
    outputs.bloomHistoryValid = inputs.bloomHistoryValid;
    outputs.bloomTemporalWarmupFrames = inputs.bloomTemporalWarmupFrames;

    if (inputs.hasVelocity && inputs.bloomTemporalShader != nullptr
        && inputs.bloomTemporalTarget != nullptr && inputs.bloomHistoryTarget != nullptr
        && inputs.bloomTemporalTarget->srvCpuHandle != 0)
    {
        SceneRenderTrace::Scope bloomTemporalScope("bloom temporal");
        const float bloomWarmupFactor = inputs.bloomHistoryValid
            ? std::min(1.0f, static_cast<float>(inputs.bloomTemporalWarmupFrames) / 4.0f)
            : 0.0f;
        inputs.bloomTemporalShader->Use(false, false);
        inputs.bloomTemporalShader->SetFloat("uBlendFactor", inputs.bloomTemporalBlendFactor);
        inputs.bloomTemporalShader->SetFloat("uSameUvBlendFactor", inputs.bloomSameUvBlendFactor);
        inputs.bloomTemporalShader->SetFloat("uHistoryValid", inputs.bloomHistoryValid ? 1.0f : 0.0f);
        inputs.bloomTemporalShader->SetFloat("uDepthThreshold", inputs.bloomDepthThreshold);
        inputs.bloomTemporalShader->SetFloat("uTexelSizeX", bloomTexelSize.x);
        inputs.bloomTemporalShader->SetFloat("uTexelSizeY", bloomTexelSize.y);
        inputs.bloomTemporalShader->SetFloat("uWarmupFactor", bloomWarmupFactor);
        inputs.bloomTemporalShader->BindTextureSlot(0, inputs.bloomBlur2Target->srvCpuHandle);
        inputs.bloomTemporalShader->BindTextureSlot(1, inputs.bloomHistoryTarget->srvCpuHandle);
        inputs.bloomTemporalShader->BindTextureSlot(2, inputs.velocitySrv);
        inputs.bloomTemporalShader->BindTextureSlot(3, inputs.depthSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.bloomTemporalShader,
            *inputs.bloomTemporalTarget,
            bloomWidth,
            bloomHeight,
            bloomClear);

        outputs.bloomSrv = inputs.bloomTemporalTarget->srvCpuHandle;
        std::swap(*inputs.bloomHistoryTarget, *inputs.bloomTemporalTarget);
        outputs.bloomHistoryValid = true;
        outputs.bloomTemporalWarmupFrames = inputs.bloomTemporalWarmupFrames + 1;
        bloomTemporalScope.Success();
    }

    bloomSection.Success();
    return true;
}

bool BloomTonemapPass::ExecuteDisplayResBloom(
    const PostProcessContext& context,
    const DisplayResBloomInputs& inputs,
    DisplayResBloomOutputs& outputs)
{
    if (inputs.hdrColorSrv == 0 || inputs.bloomExtractShader == nullptr
        || inputs.bloomBlurShader == nullptr || inputs.bloomExtractTarget == nullptr
        || inputs.bloomBlurTarget == nullptr || inputs.bloomBlur2Target == nullptr
        || inputs.displayWidth <= 0 || inputs.displayHeight <= 0)
    {
        return false;
    }

    SceneRenderTrace::Scope bloomScope("dlss display bloom");
    const int bloomWidth = std::max(1, inputs.displayWidth / 2);
    const int bloomHeight = std::max(1, inputs.displayHeight / 2);
    const glm::vec2 displayTexelSize(
        1.0f / static_cast<float>(inputs.displayWidth),
        1.0f / static_cast<float>(inputs.displayHeight));
    const glm::vec2 bloomTexelSize(
        1.0f / static_cast<float>(bloomWidth),
        1.0f / static_cast<float>(bloomHeight));
    const float bloomClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    inputs.bloomExtractShader->SetInt("uHdrColor", 0);
    inputs.bloomExtractShader->SetFloat("uThreshold", inputs.bloomThreshold);
    inputs.bloomExtractShader->SetFloat("uSoftKnee", inputs.bloomSoftKnee);
    inputs.bloomExtractShader->SetFloat("uExposure", inputs.exposure);
    inputs.bloomExtractShader->SetFloat("uFullTexelSizeX", displayTexelSize.x);
    inputs.bloomExtractShader->SetFloat("uFullTexelSizeY", displayTexelSize.y);
    inputs.bloomExtractShader->SetFloat(
        "uUseMaterialGbuffer", inputs.useMaterialGbuffer ? 1.0f : 0.0f);
    inputs.bloomExtractShader->BindTextureSlot(0, inputs.hdrColorSrv);
    if (inputs.useMaterialGbuffer)
    {
        inputs.bloomExtractShader->BindTextureSlot(1, inputs.material0Srv);
        inputs.bloomExtractShader->BindTextureSlot(2, inputs.material1Srv);
    }
    context.draw.DrawFullscreenToTarget(
        *inputs.bloomExtractShader,
        *inputs.bloomExtractTarget,
        bloomWidth,
        bloomHeight,
        bloomClear);

    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlurTarget,
        inputs.bloomExtractTarget->srvCpuHandle,
        bloomTexelSize.x,
        0.0f,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlur2Target,
        inputs.bloomBlurTarget->srvCpuHandle,
        0.0f,
        bloomTexelSize.y,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlurTarget,
        inputs.bloomBlur2Target->srvCpuHandle,
        bloomTexelSize.x,
        0.0f,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);
    DrawBloomBlurPass(
        context,
        *inputs.bloomBlurShader,
        *inputs.bloomBlur2Target,
        inputs.bloomBlurTarget->srvCpuHandle,
        0.0f,
        bloomTexelSize.y,
        inputs.bloomBlurRadius,
        bloomWidth,
        bloomHeight,
        bloomClear);

    outputs.bloomSrv = inputs.bloomBlur2Target->srvCpuHandle;
    outputs.bloomHistoryValid = inputs.bloomHistoryValid;
    outputs.bloomTemporalWarmupFrames = inputs.bloomTemporalWarmupFrames;

    const bool guidesMatchDisplay =
        inputs.renderWidth == inputs.displayWidth
        && inputs.renderHeight == inputs.displayHeight
        && inputs.hasVelocity
        && inputs.bloomTemporalShader != nullptr
        && inputs.bloomTemporalTarget != nullptr
        && inputs.bloomHistoryTarget != nullptr
        && inputs.bloomTemporalTarget->srvCpuHandle != 0;
    if (guidesMatchDisplay)
    {
        const float bloomWarmupFactor = inputs.bloomHistoryValid
            ? std::min(1.0f, static_cast<float>(inputs.bloomTemporalWarmupFrames) / 4.0f)
            : 0.0f;
        inputs.bloomTemporalShader->Use(false, false);
        inputs.bloomTemporalShader->SetFloat("uBlendFactor", inputs.bloomTemporalBlendFactor);
        inputs.bloomTemporalShader->SetFloat("uSameUvBlendFactor", inputs.bloomSameUvBlendFactor);
        inputs.bloomTemporalShader->SetFloat("uHistoryValid", inputs.bloomHistoryValid ? 1.0f : 0.0f);
        inputs.bloomTemporalShader->SetFloat("uDepthThreshold", inputs.bloomDepthThreshold);
        inputs.bloomTemporalShader->SetFloat("uTexelSizeX", bloomTexelSize.x);
        inputs.bloomTemporalShader->SetFloat("uTexelSizeY", bloomTexelSize.y);
        inputs.bloomTemporalShader->SetFloat("uWarmupFactor", bloomWarmupFactor);
        inputs.bloomTemporalShader->BindTextureSlot(0, inputs.bloomBlur2Target->srvCpuHandle);
        inputs.bloomTemporalShader->BindTextureSlot(1, inputs.bloomHistoryTarget->srvCpuHandle);
        inputs.bloomTemporalShader->BindTextureSlot(2, inputs.velocitySrv);
        inputs.bloomTemporalShader->BindTextureSlot(3, inputs.depthSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.bloomTemporalShader,
            *inputs.bloomTemporalTarget,
            bloomWidth,
            bloomHeight,
            bloomClear);
        outputs.bloomSrv = inputs.bloomTemporalTarget->srvCpuHandle;
        std::swap(*inputs.bloomHistoryTarget, *inputs.bloomTemporalTarget);
        outputs.bloomHistoryValid = true;
        outputs.bloomTemporalWarmupFrames = inputs.bloomTemporalWarmupFrames + 1;
    }

    bloomScope.Success();
    return true;
}

void BloomTonemapPass::ExecuteTonemapDlssDisplay(
    const PostProcessContext& context,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight,
    const std::uintptr_t dlssOutputSrv,
    const std::uintptr_t displayBloomSrv,
    const float exposure,
    const int tonemapMode,
    const float bloomIntensity,
    Shader* tonemapShader)
{
    if (tonemapShader == nullptr || dlssOutputSrv == 0 || viewportWidth <= 0 || viewportHeight <= 0)
    {
        return;
    }

    SceneRenderTrace::Scope tonemapScope("tonemap (dlss display)");
    const int bloomHalfWidth = std::max(1, viewportWidth / 2);
    const int bloomHalfHeight = std::max(1, viewportHeight / 2);
    const glm::vec2 bloomTexelSize(
        1.0f / static_cast<float>(bloomHalfWidth),
        1.0f / static_cast<float>(bloomHalfHeight));
    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    tonemapShader->Use(false, true);
    tonemapShader->SetInt("uHdrColor", 0);
    tonemapShader->SetFloat("uExposure", exposure);
    tonemapShader->SetInt("uTonemapMode", tonemapMode);
    tonemapShader->SetInt("uUseBloom", displayBloomSrv != 0 ? 1 : 0);
    tonemapShader->SetFloat("uBloomIntensity", bloomIntensity);
    tonemapShader->SetFloat("uBloomTexelSizeX", bloomTexelSize.x * 2.0f);
    tonemapShader->SetFloat("uBloomTexelSizeY", bloomTexelSize.y * 2.0f);
    tonemapShader->SetInt("uBloom", 1);
    tonemapShader->BindTextureSlot(0, dlssOutputSrv);
    if (displayBloomSrv != 0)
    {
        tonemapShader->BindTextureSlot(1, displayBloomSrv);
    }
    context.draw.DrawFullscreenPass(*tonemapShader, true);
    tonemapScope.Success();
}

void BloomTonemapPass::ExecuteTonemapToLdrTarget(
    const PostProcessContext& context,
    const TonemapPassInputs& inputs)
{
    if (inputs.tonemapShader == nullptr || inputs.ldrTonemapTarget == nullptr
        || inputs.hdrColorSrv == 0)
    {
        return;
    }

    const float ldrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    inputs.tonemapShader->Use(false, true);
    inputs.tonemapShader->SetInt("uHdrColor", 0);
    inputs.tonemapShader->SetFloat("uExposure", inputs.exposure);
    inputs.tonemapShader->SetInt("uTonemapMode", inputs.tonemapMode);
    inputs.tonemapShader->SetInt("uUseBloom", inputs.bloomEnabled ? 1 : 0);
    inputs.tonemapShader->SetFloat("uBloomIntensity", inputs.bloomIntensity);
    inputs.tonemapShader->SetFloat("uBloomTexelSizeX", inputs.texelSize.x * 2.0f);
    inputs.tonemapShader->SetFloat("uBloomTexelSizeY", inputs.texelSize.y * 2.0f);
    inputs.tonemapShader->SetInt("uBloom", 1);
    inputs.tonemapShader->BindTextureSlot(0, inputs.hdrColorSrv);
    if (inputs.bloomEnabled && inputs.bloomSrv != 0)
    {
        inputs.tonemapShader->BindTextureSlot(1, inputs.bloomSrv);
    }
    context.draw.DrawFullscreenToTarget(
        *inputs.tonemapShader,
        *inputs.ldrTonemapTarget,
        context.renderWidth,
        context.renderHeight,
        ldrClear,
        true);
}

void BloomTonemapPass::ExecuteTonemapToViewport(
    const PostProcessContext& context,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight,
    const TonemapPassInputs& inputs)
{
    if (inputs.tonemapShader == nullptr || inputs.hdrColorSrv == 0)
    {
        return;
    }

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.tonemapShader->Use(false, true);
    inputs.tonemapShader->SetInt("uHdrColor", 0);
    inputs.tonemapShader->SetFloat("uExposure", inputs.exposure);
    inputs.tonemapShader->SetInt("uTonemapMode", inputs.tonemapMode);
    inputs.tonemapShader->SetInt("uUseBloom", inputs.bloomEnabled ? 1 : 0);
    inputs.tonemapShader->SetFloat("uBloomIntensity", inputs.bloomIntensity);
    inputs.tonemapShader->SetFloat("uBloomTexelSizeX", inputs.texelSize.x * 2.0f);
    inputs.tonemapShader->SetFloat("uBloomTexelSizeY", inputs.texelSize.y * 2.0f);
    inputs.tonemapShader->SetInt("uBloom", 1);
    inputs.tonemapShader->BindTextureSlot(0, inputs.hdrColorSrv);
    if (inputs.bloomEnabled && inputs.bloomSrv != 0)
    {
        inputs.tonemapShader->BindTextureSlot(1, inputs.bloomSrv);
    }
    context.draw.DrawFullscreenPass(*inputs.tonemapShader, true);
}
