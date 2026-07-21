#include "engine/rendering/post/AntiAliasingPass.h"

#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"

#include <utility>

bool AntiAliasingPass::NeedsLdrIntermediate(
    const bool useFxaa,
    const bool useSmaa,
    const bool useSsaa,
    const int renderWidth,
    const int renderHeight,
    const int storedViewportWidth,
    const int storedViewportHeight)
{
    return useFxaa || useSmaa
        || (useSsaa && storedViewportWidth > 0 && storedViewportHeight > 0
            && (renderWidth != storedViewportWidth || renderHeight != storedViewportHeight));
}

void AntiAliasingPass::ExecuteTaa(
    const PostProcessContext& context,
    const TaaPassInputs& inputs,
    TaaPassOutputs& outputs)
{
    outputs.ran = false;
    outputs.hdrColorSrv = inputs.hdrColorSrv;
    outputs.taaHistoryValid = inputs.taaHistoryValid;

    if (!inputs.useTaa || inputs.taaShader == nullptr || inputs.taaResolveTarget == nullptr
        || inputs.taaHistoryTarget == nullptr || inputs.sceneFramebuffer == nullptr)
    {
        return;
    }

    if (inputs.taaResolveTarget->srvCpuHandle == 0 || !inputs.sceneFramebuffer->HasVelocity())
    {
        return;
    }

    const int renderWidth = context.renderWidth;
    const int renderHeight = context.renderHeight;
    if (renderWidth <= 0 || renderHeight <= 0)
    {
        return;
    }

    SceneRenderTrace::Section taaSection("aa-taa");
    SceneRenderTrace::Scope taaScope("taa resolve");

    const glm::mat4 invViewProjection =
        glm::inverse(inputs.unjitteredProjectionMatrix * inputs.viewMatrix);
    const glm::mat4 prevViewProjection = inputs.motionVectorState.historyValid
        ? inputs.motionVectorState.prevViewProjection
        : inputs.unjitteredProjectionMatrix * inputs.viewMatrix;
    const float hdrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    inputs.taaShader->Use(false, false);
    inputs.taaShader->SetMat4("uInvViewProj", invViewProjection);
    inputs.taaShader->SetMat4("uPrevViewProj", prevViewProjection);
    inputs.taaShader->SetFloat("uBlendFactor", inputs.taaBlendFactor);
    inputs.taaShader->SetFloat("uHistoryValid", inputs.taaHistoryValid ? 1.0f : 0.0f);
    inputs.taaShader->SetFloat("uTexelSizeX", inputs.texelSize.x);
    inputs.taaShader->SetFloat("uTexelSizeY", inputs.texelSize.y);
    inputs.taaShader->BindTextureSlot(0, inputs.hdrColorSrv);
    inputs.taaShader->BindTextureSlot(1, inputs.taaHistoryTarget->srvCpuHandle);
    inputs.taaShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
    inputs.taaShader->BindTextureSlot(
        3,
        inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity));
    context.draw.DrawFullscreenToTarget(
        *inputs.taaShader,
        *inputs.taaResolveTarget,
        renderWidth,
        renderHeight,
        hdrClear,
        false);

    outputs.hdrColorSrv = inputs.taaResolveTarget->srvCpuHandle;
    outputs.ran = true;
    outputs.taaHistoryValid = true;

    std::swap(*inputs.taaHistoryTarget, *inputs.taaResolveTarget);

    taaScope.Success();
    taaSection.Success();
}

void AntiAliasingPass::ExecuteLdrAntiAliasing(
    const PostProcessContext& context,
    const LdrAntiAliasingInputs& inputs)
{
    const int renderWidth = context.renderWidth;
    const int renderHeight = context.renderHeight;
    const float ldrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    auto blitLdrToViewport = [&](const std::uintptr_t sourceSrv) {
        context.draw.BindOutputTarget(
            inputs.outputTarget, inputs.viewportWidth, inputs.viewportHeight);
        inputs.downsampleShader->Use(false, true);
        inputs.downsampleShader->BindTextureSlot(0, sourceSrv);
        context.draw.DrawFullscreenPass(*inputs.downsampleShader, true);
    };

    if (inputs.useFxaa && inputs.fxaaShader != nullptr)
    {
        SceneRenderTrace::Section aaOutputSection("aa-output");
        SceneRenderTrace::Scope fxaaScope("fxaa");
        context.draw.BindOutputTarget(
            inputs.outputTarget, inputs.viewportWidth, inputs.viewportHeight);
        inputs.fxaaShader->Use(false, true);
        inputs.fxaaShader->SetFloat("uTexelSizeX", inputs.texelSize.x);
        inputs.fxaaShader->SetFloat("uTexelSizeY", inputs.texelSize.y);
        inputs.fxaaShader->SetFloat("uSubpixQuality", inputs.fxaaSubpixQuality);
        inputs.fxaaShader->SetFloat("uEdgeThreshold", inputs.fxaaEdgeThreshold);
        inputs.fxaaShader->BindTextureSlot(0, inputs.ldrTonemapSrv);
        context.draw.DrawFullscreenPass(*inputs.fxaaShader, true);
        fxaaScope.Success();
        aaOutputSection.Success();
        return;
    }

    if (inputs.useSmaa && inputs.smaaEdgeShader != nullptr && inputs.smaaNeighborShader != nullptr
        && inputs.smaaEdgeTarget != nullptr && inputs.smaaOutputTarget != nullptr)
    {
        SceneRenderTrace::Section aaOutputSection("aa-output");
        SceneRenderTrace::Scope smaaScope("smaa");

        inputs.smaaEdgeShader->Use(false, true);
        inputs.smaaEdgeShader->SetFloat("uTexelSizeX", inputs.texelSize.x);
        inputs.smaaEdgeShader->SetFloat("uTexelSizeY", inputs.texelSize.y);
        inputs.smaaEdgeShader->SetFloat("uThreshold", inputs.smaaThreshold);
        inputs.smaaEdgeShader->BindTextureSlot(0, inputs.ldrTonemapSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.smaaEdgeShader,
            *inputs.smaaEdgeTarget,
            renderWidth,
            renderHeight,
            ldrClear,
            true);

        inputs.smaaNeighborShader->Use(false, true);
        inputs.smaaNeighborShader->SetFloat("uTexelSizeX", inputs.texelSize.x);
        inputs.smaaNeighborShader->SetFloat("uTexelSizeY", inputs.texelSize.y);
        inputs.smaaNeighborShader->SetFloat(
            "uSearchSteps", static_cast<float>(inputs.smaaSearchSteps));
        inputs.smaaNeighborShader->BindTextureSlot(0, inputs.ldrTonemapSrv);
        inputs.smaaNeighborShader->BindTextureSlot(1, inputs.smaaEdgeTarget->srvCpuHandle);
        context.draw.DrawFullscreenToTarget(
            *inputs.smaaNeighborShader,
            *inputs.smaaOutputTarget,
            renderWidth,
            renderHeight,
            ldrClear,
            true);

        blitLdrToViewport(inputs.smaaOutputTarget->srvCpuHandle);
        smaaScope.Success();
        aaOutputSection.Success();
        return;
    }

    if (inputs.useSsaa
        && (renderWidth != inputs.viewportWidth || renderHeight != inputs.viewportHeight)
        && inputs.downsampleShader != nullptr)
    {
        SceneRenderTrace::Section aaOutputSection("aa-output");
        SceneRenderTrace::Scope ssaaScope("ssaa blit");
        blitLdrToViewport(inputs.ldrTonemapSrv);
        ssaaScope.Success();
        aaOutputSection.Success();
        return;
    }

    if (inputs.downsampleShader != nullptr)
    {
        SceneRenderTrace::Scope blitScope("ldr blit to viewport");
        context.draw.BindOutputTarget(
            inputs.outputTarget, inputs.viewportWidth, inputs.viewportHeight);
        inputs.downsampleShader->Use(false, true);
        inputs.downsampleShader->BindTextureSlot(0, inputs.ldrTonemapSrv);
        context.draw.DrawFullscreenPass(*inputs.downsampleShader, true);
        blitScope.Success();
    }
}

void AntiAliasingPass::ExecuteMsaaDepthResolve(
    const PostProcessContext& context,
    const MsaaDepthResolveInputs& inputs)
{
    if (inputs.sceneFramebuffer == nullptr || inputs.msaaDepthResolveShader == nullptr)
    {
        return;
    }

    if (!inputs.sceneFramebuffer->UsesMsaa())
    {
        return;
    }

    inputs.sceneFramebuffer->ResolveMsaa();

    if (inputs.sceneFramebuffer->GetMsaaDepthSrvCpuHandle() == 0)
    {
        return;
    }

    inputs.sceneFramebuffer->BeginMsaaDepthResolvePass();
    inputs.msaaDepthResolveShader->Use(false, false);
    inputs.msaaDepthResolveShader->SetInt(
        "uSampleCount",
        inputs.sceneFramebuffer->GetSampleCount());
    inputs.msaaDepthResolveShader->BindTextureSlot(
        0,
        inputs.sceneFramebuffer->GetMsaaDepthSrvCpuHandle());
    inputs.msaaDepthResolveShader->SetInt("uMsaaDepth", 0);
    context.draw.DrawFullscreenPass(*inputs.msaaDepthResolveShader, false);
    inputs.sceneFramebuffer->FinishMsaaDepthResolvePass();
}
