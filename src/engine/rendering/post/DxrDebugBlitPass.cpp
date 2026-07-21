#include "engine/rendering/post/DxrDebugBlitPass.h"

#include "engine/rendering/shaders/Shader.h"

#include <glm/glm.hpp>

void DxrDebugBlitPass::BlitDispatchSmoke(
    const PostProcessContext& context,
    const DxrDebugBlitInputs& inputs,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight)
{
    if (inputs.debugMode != RenderDebugMode::RtDispatchSmoke || inputs.dxrSmokeDebugSrv == 0
        || outputTarget == nullptr || inputs.debugChannelShader == nullptr)
    {
        return;
    }

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.debugChannelShader->Use(false, true);
    inputs.debugChannelShader->SetInt("uOutputRgb", 1);
    inputs.debugChannelShader->SetInt("uOutputAlpha", 0);
    inputs.debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
    inputs.debugChannelShader->SetInt("uInput", 0);
    inputs.debugChannelShader->BindTextureSlot(0, inputs.dxrSmokeDebugSrv);
    inputs.debugChannelShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}

void DxrDebugBlitPass::BlitPrimary(
    const PostProcessContext& context,
    const DxrDebugBlitInputs& inputs,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight)
{
    if (!IsRtPrimaryDebugMode(inputs.debugMode) || !inputs.primaryDebugBlitReady
        || inputs.dxrPrimaryOutputSrv == 0 || inputs.dxrPrimaryMetadataSrv == 0
        || outputTarget == nullptr || inputs.dxrPrimaryDebugShader == nullptr)
    {
        return;
    }

    int viewMode = 0;
    switch (inputs.debugMode)
    {
    case RenderDebugMode::RtPrimaryHit:
        viewMode = 0;
        break;
    case RenderDebugMode::RtPrimaryDepth:
        viewMode = 1;
        break;
    case RenderDebugMode::RtPrimaryNormal:
        viewMode = 2;
        break;
    default:
        return;
    }

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.dxrPrimaryDebugShader->Use(false, true);
    inputs.dxrPrimaryDebugShader->SetInt("uViewMode", viewMode);
    inputs.dxrPrimaryDebugShader->SetFloat("uMaxTraceDistance", inputs.maxTraceDistance);
    inputs.dxrPrimaryDebugShader->SetInt("uPrimaryOutput", 0);
    inputs.dxrPrimaryDebugShader->SetInt("uPrimaryMetadata", 1);
    inputs.dxrPrimaryDebugShader->BindTextureSlot(0, inputs.dxrPrimaryOutputSrv);
    inputs.dxrPrimaryDebugShader->BindTextureSlot(1, inputs.dxrPrimaryMetadataSrv);
    inputs.dxrPrimaryDebugShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}

void DxrDebugBlitPass::BlitReflection(
    const PostProcessContext& context,
    const DxrDebugBlitInputs& inputs,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight)
{
    if (!IsRtReflectionDebugMode(inputs.debugMode) || inputs.dxrReflectionSrv == 0
        || outputTarget == nullptr || inputs.debugChannelShader == nullptr)
    {
        return;
    }

    // RtSpecReplacement is rendered by the composite debug branch inside Apply(), not here.
    if (inputs.debugMode == RenderDebugMode::RtSpecReplacement)
    {
        return;
    }

    const bool showHitDistance = inputs.debugMode == RenderDebugMode::RtReflectionConfidence;
    const bool showDenoised = inputs.debugMode == RenderDebugMode::RtReflectionDenoised;

    if (showDenoised && inputs.dxrReflectionDenoisedSrv != 0
        && inputs.rtReflectionResolveShader != nullptr)
    {
        context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        inputs.rtReflectionResolveShader->Use(false, true);
        inputs.rtReflectionResolveShader->SetInt("uDenoised", 0);
        inputs.rtReflectionResolveShader->SetInt("uRaw", 1);
        inputs.rtReflectionResolveShader->SetVec2(
            "uUvScale",
            glm::vec2(inputs.dxrReflectionUvScaleX, inputs.dxrReflectionUvScaleY));
        inputs.rtReflectionResolveShader->SetFloat(
            "uMaxTraceDistance", inputs.dxrReflectionMaxTraceDistance);
        inputs.rtReflectionResolveShader->BindTextureSlot(0, inputs.dxrReflectionDenoisedSrv);
        inputs.rtReflectionResolveShader->BindTextureSlot(1, inputs.dxrReflectionSrv);
        inputs.rtReflectionResolveShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        return;
    }

    const std::uintptr_t sourceSrv = inputs.dxrReflectionSrv;

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.debugChannelShader->Use(false, true);
    inputs.debugChannelShader->SetInt("uInput", 0);
    inputs.debugChannelShader->SetInt("uOutputRgb", showHitDistance ? 0 : 1);
    inputs.debugChannelShader->SetInt("uOutputAlpha", showHitDistance ? 1 : 0);
    inputs.debugChannelShader->SetFloat(
        "uAlphaScale",
        inputs.dxrReflectionMaxTraceDistance > 0.0f
            ? 1.0f / inputs.dxrReflectionMaxTraceDistance
            : 1.0f);
    inputs.debugChannelShader->SetVec2(
        "uUvScale",
        glm::vec2(inputs.dxrReflectionUvScaleX, inputs.dxrReflectionUvScaleY));
    inputs.debugChannelShader->BindTextureSlot(0, sourceSrv);
    inputs.debugChannelShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}

void DxrDebugBlitPass::BlitShadow(
    const PostProcessContext& context,
    const DxrDebugBlitInputs& inputs,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight)
{
    if (!IsRtShadowDebugMode(inputs.debugMode) || outputTarget == nullptr
        || inputs.dxrShadowDebugShader == nullptr)
    {
        return;
    }

    const bool wantDenoised = inputs.debugMode == RenderDebugMode::RtShadowDenoised;
    const bool useDenoised = wantDenoised && inputs.dxrShadowDenoisedSrv != 0;
    const std::uintptr_t sourceSrv =
        useDenoised ? inputs.dxrShadowDenoisedSrv : inputs.dxrShadowPenumbraSrv;
    if (sourceSrv == 0)
    {
        return;
    }

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.dxrShadowDebugShader->Use(false, true);
    inputs.dxrShadowDebugShader->SetInt("uInput", 0);
    inputs.dxrShadowDebugShader->SetInt("uRawPenumbra", useDenoised ? 0 : 1);
    inputs.dxrShadowDebugShader->SetVec2(
        "uUvScale", glm::vec2(inputs.dxrShadowUvScaleX, inputs.dxrShadowUvScaleY));
    inputs.dxrShadowDebugShader->BindTextureSlot(0, sourceSrv);
    inputs.dxrShadowDebugShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}

void DxrDebugBlitPass::BlitGi(
    const PostProcessContext& context,
    const DxrDebugBlitInputs& inputs,
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight)
{
    if (!IsRtGiDebugMode(inputs.debugMode) || outputTarget == nullptr
        || inputs.debugChannelShader == nullptr)
    {
        return;
    }

    if (inputs.debugMode == RenderDebugMode::RtGiInject)
    {
        const std::uintptr_t giInjectSrv =
            inputs.dxrGiDenoisedSrv != 0 ? inputs.dxrGiDenoisedSrv : inputs.dxrGiRawSrv;
        if (giInjectSrv == 0 || !inputs.sceneHasMaterialGbuffer
            || inputs.dxrGiInjectShader == nullptr)
        {
            return;
        }

        context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        inputs.dxrGiInjectShader->Use(false, true);
        inputs.dxrGiInjectShader->SetInt("uIndirectMap", 0);
        inputs.dxrGiInjectShader->SetInt("uGiDenoisedMap", 1);
        inputs.dxrGiInjectShader->SetInt("uDepthMap", 2);
        inputs.dxrGiInjectShader->SetInt("uMaterial0Map", 3);
        inputs.dxrGiInjectShader->SetInt("uMaterial1Map", 4);
        inputs.dxrGiInjectShader->SetVec2(
            "uGiUvScale", glm::vec2(inputs.dxrGiUvScaleX, inputs.dxrGiUvScaleY));
        inputs.dxrGiInjectShader->SetFloat("uStrength", inputs.dxrGiStrength);
        inputs.dxrGiInjectShader->SetInt("uDebugGiInject", 1);
        inputs.dxrGiInjectShader->BindTextureSlot(0, inputs.sceneIndirectSrv);
        inputs.dxrGiInjectShader->BindTextureSlot(1, giInjectSrv);
        inputs.dxrGiInjectShader->BindTextureSlot(2, inputs.sceneDepthSrv);
        inputs.dxrGiInjectShader->BindTextureSlot(3, inputs.sceneMaterial0Srv);
        inputs.dxrGiInjectShader->BindTextureSlot(4, inputs.sceneMaterial1Srv);
        inputs.dxrGiInjectShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        return;
    }

    const bool wantDenoised = inputs.debugMode == RenderDebugMode::RtGiDenoised;
    const std::uintptr_t sourceSrv =
        (wantDenoised && inputs.dxrGiDenoisedSrv != 0)
            ? inputs.dxrGiDenoisedSrv
            : inputs.dxrGiRawSrv;
    if (sourceSrv == 0)
    {
        return;
    }

    context.draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    inputs.debugChannelShader->Use(false, true);
    inputs.debugChannelShader->SetInt("uInput", 0);
    inputs.debugChannelShader->SetInt("uOutputRgb", 1);
    inputs.debugChannelShader->SetInt("uOutputAlpha", 0);
    inputs.debugChannelShader->SetFloat("uAlphaScale", 1.0f);
    inputs.debugChannelShader->SetVec2(
        "uUvScale", glm::vec2(inputs.dxrGiUvScaleX, inputs.dxrGiUvScaleY));
    inputs.debugChannelShader->BindTextureSlot(0, sourceSrv);
    inputs.debugChannelShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}
