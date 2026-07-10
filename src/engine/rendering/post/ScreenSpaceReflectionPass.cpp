#include "engine/rendering/post/ScreenSpaceReflectionPass.h"

#include "engine/platform/SceneRenderTrace.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

#include <algorithm>

void ScreenSpaceReflectionPass::Execute(
    const PostProcessContext& context,
    const ScreenSpaceReflectionPassInputs& inputs,
    ScreenSpaceReflectionPassOutputs& outputs)
{
    outputs.indirectCompositeSrv = inputs.indirectLightingSrv;
    outputs.ssrFrameIndex = inputs.ssrFrameIndex;
    outputs.ssrHistoryValid = inputs.ssrHistoryValid;

    const float radianceClear[] = {0.0f, 0.0f, 0.0f, 0.0f};

    const bool runSsrSceneColorAssembly =
        inputs.wantsSsr &&
        !inputs.pbrDebugActive &&
        inputs.sceneHasSplitLighting &&
        inputs.sceneHasShadowFactor &&
        inputs.ssrSceneColorTarget != nullptr &&
        inputs.ssrSceneColorTarget->resource != nullptr;

    outputs.ssrSceneColorRanLastFrame = runSsrSceneColorAssembly;

    if (runSsrSceneColorAssembly)
    {
        SceneRenderTrace::Scope ssrSceneColorScope("ssr scene color");
        const bool useBloomInReflections =
            inputs.bloomEnabled && inputs.prevFrameBloomSrv != 0;
        inputs.ssrSceneColorShader->Use(false);
        inputs.ssrSceneColorShader->SetInt("uDirectLighting", 0);
        inputs.ssrSceneColorShader->SetInt("uSunShadowMap", 1);
        inputs.ssrSceneColorShader->SetInt("uDepthMap", 2);
        inputs.ssrSceneColorShader->SetInt("uIndirectLighting", 3);
        inputs.ssrSceneColorShader->SetInt("uPrevBloom", 4);
        inputs.ssrSceneColorShader->SetInt(
            "uUseShadowFactor",
            inputs.useShadowFactorComposite ? 1 : 0);
        inputs.ssrSceneColorShader->SetInt("uUseIndirect", 1);
        inputs.ssrSceneColorShader->SetFloat(
            "uBloomIntensity",
            useBloomInReflections ? inputs.bloomIntensity : 0.0f);
        inputs.ssrSceneColorShader->BindTextureSlot(0, inputs.directLightingSrv);
        inputs.ssrSceneColorShader->BindTextureSlot(1, inputs.shadowFactorSrv);
        inputs.ssrSceneColorShader->BindTextureSlot(2, inputs.depthSrv);
        inputs.ssrSceneColorShader->BindTextureSlot(3, inputs.indirectLightingSrv);
        inputs.ssrSceneColorShader->BindTextureSlot(
            4,
            useBloomInReflections ? inputs.prevFrameBloomSrv : inputs.directLightingSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssrSceneColorShader,
            *inputs.ssrSceneColorTarget,
            inputs.ssrSceneColorTarget->width,
            inputs.ssrSceneColorTarget->height,
            radianceClear);
        ssrSceneColorScope.Success();
    }

    const bool runSsrTrace =
        inputs.wantsSsr &&
        runSsrSceneColorAssembly &&
        !inputs.pbrDebugActive &&
        inputs.sceneHasSplitLighting &&
        inputs.sceneHasGeometryNormals &&
        inputs.sceneHasMaterialGbuffer &&
        inputs.ssrSceneColorTarget != nullptr &&
        inputs.ssrSceneColorTarget->resource != nullptr &&
        inputs.ssrTraceTarget != nullptr &&
        inputs.ssrTraceTarget->resource != nullptr &&
        (inputs.ssrEnabled || inputs.isSsrTraceDebug || inputs.isSsrDenoiseDebug
         || inputs.isSsrCompositeDebug);

    outputs.ssrTraceRanLastFrame = runSsrTrace;
    outputs.lastSsrSpatialSrv = 0;
    outputs.lastSsrVarianceSrv = 0;
    outputs.lastSsrDenoiseSrv = 0;
    outputs.lastSsrResolvedSrv = 0;

    if (runSsrTrace)
    {
        SceneRenderTrace::Scope ssrTraceScope("ssr trace");
        inputs.ssrTraceShader->Use(false);
        inputs.ssrTraceShader->SetInt("uDepthMap", 0);
        inputs.ssrTraceShader->SetInt("uNormalMap", 1);
        inputs.ssrTraceShader->SetInt("uMaterial0Map", 2);
        inputs.ssrTraceShader->SetInt("uSceneColorMap", 3);
        inputs.ssrTraceShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssrTraceShader->SetMat4("uProjection", inputs.projectionMatrix);
        inputs.ssrTraceShader->SetMat4("uView", inputs.viewMatrix);
        inputs.ssrTraceShader->SetFloat("uMaxTraceDistance", inputs.ssrMaxTraceDistance);
        inputs.ssrTraceShader->SetInt("uStepCount", inputs.ssrStepCount);
        inputs.ssrTraceShader->SetFloat("uThickness", inputs.ssrThickness);
        inputs.ssrTraceShader->SetFloat("uRoughnessCutoff", inputs.ssrRoughnessCutoff);
        inputs.ssrTraceShader->SetFloat("uFrameIndex", static_cast<float>(outputs.ssrFrameIndex));
        inputs.ssrTraceShader->SetFloat("uStepExponent", inputs.ssrStepExponent);
        inputs.ssrTraceShader->SetInt("uSampleCount", inputs.ssrSampleCount);
        inputs.ssrTraceShader->SetVec2(
            "uTexelSize",
            glm::vec2(
                1.0f / static_cast<float>(std::max(inputs.ssrTraceTarget->width, 1)),
                1.0f / static_cast<float>(std::max(inputs.ssrTraceTarget->height, 1))));
        inputs.ssrTraceShader->BindTextureSlot(0, inputs.depthSrv);
        inputs.ssrTraceShader->BindTextureSlot(1, inputs.normalSrv);
        inputs.ssrTraceShader->BindTextureSlot(2, inputs.material0Srv);
        inputs.ssrTraceShader->BindTextureSlot(3, inputs.ssrSceneColorTarget->srvCpuHandle);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssrTraceShader,
            *inputs.ssrTraceTarget,
            inputs.ssrTraceTarget->width,
            inputs.ssrTraceTarget->height,
            radianceClear);
        ++outputs.ssrFrameIndex;
        ssrTraceScope.Success();
    }

    const bool runSsrDenoise =
        runSsrTrace &&
        (inputs.ssrDenoiseEnabled || inputs.isSsrDenoiseDebug) &&
        inputs.ssrSpatialTarget != nullptr &&
        inputs.ssrSpatialTarget->resource != nullptr &&
        inputs.ssrSpatialBlurTarget != nullptr &&
        inputs.ssrSpatialBlurTarget->resource != nullptr &&
        inputs.ssrVarianceHistoryTarget != nullptr &&
        inputs.ssrVarianceHistoryTarget->resource != nullptr &&
        inputs.ssrVarianceTemporalTarget != nullptr &&
        inputs.ssrVarianceTemporalTarget->resource != nullptr;

    outputs.ssrDenoiseRanLastFrame = runSsrDenoise;

    std::uintptr_t ssrDenoiseInputSrv = inputs.ssrTraceTarget != nullptr
        ? inputs.ssrTraceTarget->srvCpuHandle
        : 0;
    std::uintptr_t ssrVarianceSrv = 0;
    if (runSsrDenoise)
    {
        SceneRenderTrace::Scope ssrSvgfScope("ssr svgf");
        const float temporalClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        const glm::vec2 traceTexelSize(
            1.0f / static_cast<float>(inputs.ssrTraceTarget->width),
            1.0f / static_cast<float>(inputs.ssrTraceTarget->height));
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        if (inputs.sceneFramebuffer != nullptr)
        {
            inputs.sceneFramebuffer->RestoreDepthShaderResource();
        }

        const bool useMotionVectors = inputs.sceneHasVelocity;
        const bool runSsrTemporal =
            inputs.ssrHistoryTarget != nullptr &&
            inputs.ssrHistoryTarget->resource != nullptr &&
            inputs.ssrTemporalTarget != nullptr &&
            inputs.ssrTemporalTarget->resource != nullptr;

        outputs.ssrTemporalRanLastFrame = runSsrTemporal;

        if (runSsrTemporal && useMotionVectors)
        {
            SceneRenderTrace::Scope ssrTemporalScope("ssr svgf temporal color");
            const glm::mat4 unjitteredProjection = inputs.unjitteredProjectionMatrix;
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * inputs.viewMatrix);
            const glm::mat4 prevViewProj = inputs.motionVectorState.historyValid
                ? inputs.motionVectorState.prevViewProjection
                : unjitteredProjection * inputs.viewMatrix;

            inputs.ssrSvgfTemporalShader->Use(false);
            inputs.ssrSvgfTemporalShader->SetInt("uCurrentTrace", 0);
            inputs.ssrSvgfTemporalShader->SetInt("uHistoryTrace", 1);
            inputs.ssrSvgfTemporalShader->SetInt("uVelocity", 2);
            inputs.ssrSvgfTemporalShader->SetInt("uDepth", 3);
            inputs.ssrSvgfTemporalShader->SetInt("uNormalMap", 4);
            inputs.ssrSvgfTemporalShader->SetInt("uHistoryDepth", 5);
            inputs.ssrSvgfTemporalShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
            inputs.ssrSvgfTemporalShader->SetMat4("uInvViewProj", invViewProjCurr);
            inputs.ssrSvgfTemporalShader->SetMat4("uPrevViewProj", prevViewProj);
            inputs.ssrSvgfTemporalShader->SetFloat("uBlendFactor", inputs.ssrTemporalBlendFactor);
            inputs.ssrSvgfTemporalShader->SetFloat("uSameUvBlendFactor", inputs.ssrSameUvBlendFactor);
            inputs.ssrSvgfTemporalShader->SetFloat("uHistoryValid", outputs.ssrHistoryValid ? 1.0f : 0.0f);
            inputs.ssrSvgfTemporalShader->SetFloat("uDepthThreshold", inputs.ssrDepthThreshold);
            inputs.ssrSvgfTemporalShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            inputs.ssrSvgfTemporalShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(1, inputs.ssrHistoryTarget->srvCpuHandle);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(2, inputs.velocitySrv);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(3, inputs.depthSrv);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(4, inputs.normalSrv);
            inputs.ssrSvgfTemporalShader->BindTextureSlot(5, inputs.ssrHistoryDepthTarget->srvCpuHandle);
            context.draw.DrawFullscreenToTarget(
                *inputs.ssrSvgfTemporalShader,
                *inputs.ssrTemporalTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                temporalClear);

            inputs.giDepthHistoryShader->Use(false);
            inputs.giDepthHistoryShader->SetInt("uDepth", 0);
            inputs.giDepthHistoryShader->BindTextureSlot(0, inputs.depthSrv);
            context.draw.DrawFullscreenToTarget(
                *inputs.giDepthHistoryShader,
                *inputs.ssrHistoryDepthTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                temporalClear);
            ssrTemporalScope.Success();
        }
        else if (runSsrTemporal)
        {
            SceneRenderTrace::Scope ssrTemporalScope("ssr svgf temporal color");
            const glm::mat4 unjitteredProjection = inputs.unjitteredProjectionMatrix;
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * inputs.viewMatrix);
            const glm::mat4 prevViewProj = inputs.motionVectorState.historyValid
                ? inputs.motionVectorState.prevViewProjection
                : unjitteredProjection * inputs.viewMatrix;

            inputs.temporalReprojectShader->Use(false);
            inputs.temporalReprojectShader->SetInt("uCurrentRadiance", 0);
            inputs.temporalReprojectShader->SetInt("uHistoryRadiance", 1);
            inputs.temporalReprojectShader->SetInt("uDepth", 2);
            inputs.temporalReprojectShader->SetInt("uHistoryDepth", 3);
            inputs.temporalReprojectShader->SetMat4("uInvViewProj", invViewProjCurr);
            inputs.temporalReprojectShader->SetMat4("uPrevViewProj", prevViewProj);
            inputs.temporalReprojectShader->SetFloat("uBlendFactor", inputs.ssrTemporalBlendFactor);
            inputs.temporalReprojectShader->SetFloat(
                "uHistoryValid",
                outputs.ssrHistoryValid && inputs.motionVectorState.historyValid ? 1.0f : 0.0f);
            inputs.temporalReprojectShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            inputs.temporalReprojectShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            inputs.temporalReprojectShader->SetFloat("uDepthRejectThreshold", inputs.ssrDepthThreshold);
            inputs.temporalReprojectShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            inputs.temporalReprojectShader->BindTextureSlot(1, inputs.ssrHistoryTarget->srvCpuHandle);
            inputs.temporalReprojectShader->BindTextureSlot(2, inputs.depthSrv);
            inputs.temporalReprojectShader->BindTextureSlot(3, inputs.ssrHistoryDepthTarget->srvCpuHandle);
            context.draw.DrawFullscreenToTarget(
                *inputs.temporalReprojectShader,
                *inputs.ssrTemporalTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                temporalClear);

            inputs.giDepthHistoryShader->Use(false);
            inputs.giDepthHistoryShader->SetInt("uDepth", 0);
            inputs.giDepthHistoryShader->BindTextureSlot(0, inputs.depthSrv);
            context.draw.DrawFullscreenToTarget(
                *inputs.giDepthHistoryShader,
                *inputs.ssrHistoryDepthTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                temporalClear);
            ssrTemporalScope.Success();
        }

        if (runSsrTemporal)
        {
            SceneRenderTrace::Scope ssrVarianceScope("ssr svgf temporal variance");
            const glm::mat4 unjitteredProjection = inputs.unjitteredProjectionMatrix;
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * inputs.viewMatrix);
            const glm::mat4 prevViewProj = inputs.motionVectorState.historyValid
                ? inputs.motionVectorState.prevViewProjection
                : unjitteredProjection * inputs.viewMatrix;

            inputs.ssrSvgfVarianceTemporalShader->Use(false);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uCurrentTrace", 0);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uFilteredColor", 1);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uHistoryVariance", 2);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uVelocity", 3);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uDepth", 4);
            inputs.ssrSvgfVarianceTemporalShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
            inputs.ssrSvgfVarianceTemporalShader->SetMat4("uInvViewProj", invViewProjCurr);
            inputs.ssrSvgfVarianceTemporalShader->SetMat4("uPrevViewProj", prevViewProj);
            inputs.ssrSvgfVarianceTemporalShader->SetInt("uUseMotionVectors", useMotionVectors ? 1 : 0);
            inputs.ssrSvgfVarianceTemporalShader->SetFloat("uBlendFactor", inputs.ssrTemporalBlendFactor);
            inputs.ssrSvgfVarianceTemporalShader->SetFloat("uHistoryValid", outputs.ssrHistoryValid ? 1.0f : 0.0f);
            inputs.ssrSvgfVarianceTemporalShader->SetFloat("uDepthThreshold", inputs.ssrDepthThreshold);
            inputs.ssrSvgfVarianceTemporalShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            inputs.ssrSvgfVarianceTemporalShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(1, inputs.ssrTemporalTarget->srvCpuHandle);
            inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(2, inputs.ssrVarianceHistoryTarget->srvCpuHandle);
            if (useMotionVectors)
            {
                inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(3, inputs.velocitySrv);
            }
            else
            {
                inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(3, inputs.depthSrv);
            }
            inputs.ssrSvgfVarianceTemporalShader->BindTextureSlot(4, inputs.depthSrv);
            context.draw.DrawFullscreenToTarget(
                *inputs.ssrSvgfVarianceTemporalShader,
                *inputs.ssrVarianceTemporalTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                temporalClear);
            ssrVarianceScope.Success();

            std::swap(*inputs.ssrHistoryTarget, *inputs.ssrTemporalTarget);
            std::swap(*inputs.ssrVarianceHistoryTarget, *inputs.ssrVarianceTemporalTarget);
            outputs.ssrHistoryValid = true;
            ssrDenoiseInputSrv = inputs.ssrHistoryTarget->srvCpuHandle;
            ssrVarianceSrv = inputs.ssrVarianceHistoryTarget->srvCpuHandle;
            outputs.lastSsrTemporalSrv = ssrDenoiseInputSrv;
            outputs.lastSsrVarianceSrv = ssrVarianceSrv;
        }

        SceneRenderTrace::Scope ssrAtrousScope("ssr svgf atrous");
        inputs.ssrSvgfAtrousShader->Use(false);
        inputs.ssrSvgfAtrousShader->SetInt("uColor", 0);
        inputs.ssrSvgfAtrousShader->SetInt("uVariance", 1);
        inputs.ssrSvgfAtrousShader->SetInt("uDepthMap", 2);
        inputs.ssrSvgfAtrousShader->SetInt("uNormalMap", 3);
        inputs.ssrSvgfAtrousShader->SetInt("uMaterial0Map", 4);
        inputs.ssrSvgfAtrousShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssrSvgfAtrousShader->SetVec2("uTexelSize", traceTexelSize);
        inputs.ssrSvgfAtrousShader->SetFloat("uDepthThreshold", inputs.ssrSpatialDepthThreshold);
        inputs.ssrSvgfAtrousShader->SetFloat("uBlurSpread", inputs.ssrSpatialBlurSpread);
        inputs.ssrSvgfAtrousShader->SetFloat("uRoughnessSpreadMin", inputs.ssrRoughnessSpreadMin);
        inputs.ssrSvgfAtrousShader->SetFloat("uRoughnessSpreadMax", inputs.ssrRoughnessSpreadMax);
        inputs.ssrSvgfAtrousShader->SetFloat("uNormalPower", 16.0f);
        inputs.ssrSvgfAtrousShader->SetFloat("uPhiEpsilon", inputs.ssrSvgfPhiEpsilon);
        inputs.ssrSvgfAtrousShader->SetFloat("uFilterStrength", inputs.ssrSvgfFilterStrength);

        static constexpr float kSsrSvgfAtrousStepScales[] = {1.0f, 2.0f, 4.0f, 8.0f};
        std::uintptr_t atrousInputSrv = ssrDenoiseInputSrv;
        bool writeToBlurTarget = true;
        bool recordedSpatialDebug = false;
        for (const float stepScale : kSsrSvgfAtrousStepScales)
        {
            PostProcessTarget& atrousOutputTarget = writeToBlurTarget
                ? *inputs.ssrSpatialBlurTarget
                : *inputs.ssrSpatialTarget;
            inputs.ssrSvgfAtrousShader->SetFloat("uStepScale", stepScale);
            inputs.ssrSvgfAtrousShader->BindTextureSlot(0, atrousInputSrv);
            inputs.ssrSvgfAtrousShader->BindTextureSlot(1, ssrVarianceSrv);
            inputs.ssrSvgfAtrousShader->BindTextureSlot(2, inputs.depthSrv);
            inputs.ssrSvgfAtrousShader->BindTextureSlot(3, inputs.normalSrv);
            inputs.ssrSvgfAtrousShader->BindTextureSlot(4, inputs.material0Srv);
            context.draw.DrawFullscreenToTarget(
                *inputs.ssrSvgfAtrousShader,
                atrousOutputTarget,
                inputs.ssrTraceTarget->width,
                inputs.ssrTraceTarget->height,
                radianceClear);
            atrousInputSrv = atrousOutputTarget.srvCpuHandle;
            if (!recordedSpatialDebug)
            {
                outputs.lastSsrSpatialSrv = atrousInputSrv;
                recordedSpatialDebug = true;
            }
            writeToBlurTarget = !writeToBlurTarget;
        }

        ssrDenoiseInputSrv = atrousInputSrv;
        ssrAtrousScope.Success();
        ssrSvgfScope.Success();
    }
    else
    {
        outputs.ssrTemporalRanLastFrame = false;
    }

    outputs.lastSsrDenoiseSrv = ssrDenoiseInputSrv;

    const bool runSsrUpscale =
        runSsrDenoise &&
        inputs.ssrTraceResolutionScale < 1.0f &&
        inputs.ssrResolvedTarget != nullptr &&
        inputs.ssrResolvedTarget->resource != nullptr;

    if (runSsrUpscale)
    {
        SceneRenderTrace::Scope ssrUpscaleScope("ssr upscale");
        inputs.ssrUpscaleShader->Use(false);
        inputs.ssrUpscaleShader->SetInt("uTraceMap", 0);
        inputs.ssrUpscaleShader->SetInt("uDepthMap", 1);
        inputs.ssrUpscaleShader->SetInt("uNormalMap", 2);
        inputs.ssrUpscaleShader->SetInt("uMaterial0Map", 3);
        inputs.ssrUpscaleShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssrUpscaleShader->SetVec2("uTexelSize", inputs.texelSize);
        inputs.ssrUpscaleShader->SetFloat("uDepthThreshold", inputs.ssrSpatialDepthThreshold);
        inputs.ssrUpscaleShader->SetFloat("uNormalPower", 8.0f);
        inputs.ssrUpscaleShader->SetFloat("uRoughnessSpreadMin", 0.75f);
        inputs.ssrUpscaleShader->SetFloat("uRoughnessSpreadMax", 1.75f);
        inputs.ssrUpscaleShader->BindTextureSlot(0, ssrDenoiseInputSrv);
        inputs.ssrUpscaleShader->BindTextureSlot(1, inputs.depthSrv);
        inputs.ssrUpscaleShader->BindTextureSlot(2, inputs.normalSrv);
        inputs.ssrUpscaleShader->BindTextureSlot(3, inputs.material0Srv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssrUpscaleShader,
            *inputs.ssrResolvedTarget,
            context.renderWidth,
            context.renderHeight,
            radianceClear);
        outputs.lastSsrResolvedSrv = inputs.ssrResolvedTarget->srvCpuHandle;
        ssrUpscaleScope.Success();
    }
    else if (runSsrDenoise && ssrDenoiseInputSrv != 0)
    {
        outputs.lastSsrResolvedSrv = ssrDenoiseInputSrv;
    }
    else if (runSsrTrace && inputs.ssrTraceTarget != nullptr
             && inputs.ssrTraceTarget->srvCpuHandle != 0)
    {
        outputs.lastSsrResolvedSrv = inputs.ssrTraceTarget->srvCpuHandle;
    }

    const bool ssrHasFreshTrace = outputs.lastSsrResolvedSrv != 0;
    const bool runSsrIndirect =
        !inputs.pbrDebugActive &&
        !inputs.rtCompositeWanted &&
        inputs.sceneHasSplitLighting &&
        inputs.sceneHasMaterialGbuffer &&
        inputs.ssrIndirectTarget != nullptr &&
        inputs.ssrIndirectTarget->resource != nullptr &&
        inputs.iblReady &&
        (inputs.ssrEnabled || inputs.isSsrCompositeDebug);

    if (runSsrIndirect)
    {
        SceneRenderTrace::Scope ssrIndirectScope("ssr indirect composite");
        const float indirectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const glm::mat4 invView = glm::inverse(inputs.viewMatrix);

        inputs.ssrIndirectShader->Use(false);
        inputs.ssrIndirectShader->SetInt("uIndirectMap", 0);
        inputs.ssrIndirectShader->SetInt("uSsrMap", 1);
        inputs.ssrIndirectShader->SetInt("uDepthMap", 2);
        inputs.ssrIndirectShader->SetInt("uNormalMap", 3);
        inputs.ssrIndirectShader->SetInt("uMaterial0Map", 4);
        inputs.ssrIndirectShader->SetInt("uMaterial1Map", 5);
        inputs.ssrIndirectShader->SetInt("uPrefilterMap", 6);
        inputs.ssrIndirectShader->SetInt("uBrdfLut", 7);
        inputs.ssrIndirectShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssrIndirectShader->SetMat4("uInvView", invView);
        inputs.ssrIndirectShader->SetFloat("uEnvironmentIntensity", inputs.environmentIntensity);
        inputs.ssrIndirectShader->SetFloat("uMaxReflectionLod", inputs.maxReflectionLod);
        inputs.ssrIndirectShader->SetFloat("uSsrStrength", inputs.ssrStrength);
        inputs.ssrIndirectShader->SetFloat("uReceiverFadeDistance", inputs.ssrMaxTraceDistance);
        inputs.ssrIndirectShader->SetInt(
            "uDebugSpecReplacement",
            inputs.ssrSpecReplacementDebug ? 1 : 0);
        inputs.ssrIndirectShader->SetInt("uHasSsrTrace", ssrHasFreshTrace ? 1 : 0);
        inputs.ssrIndirectShader->BindTextureSlot(0, inputs.indirectLightingSrv);
        inputs.ssrIndirectShader->BindTextureSlot(
            1,
            ssrHasFreshTrace ? outputs.lastSsrResolvedSrv : inputs.indirectLightingSrv);
        inputs.ssrIndirectShader->BindTextureSlot(2, inputs.depthSrv);
        inputs.ssrIndirectShader->BindTextureSlot(3, inputs.normalSrv);
        inputs.ssrIndirectShader->BindTextureSlot(4, inputs.material0Srv);
        inputs.ssrIndirectShader->BindTextureSlot(5, inputs.material1Srv);
        inputs.ssrIndirectShader->BindTextureSlot(6, inputs.prefilterMapSrv);
        inputs.ssrIndirectShader->BindTextureSlot(7, inputs.brdfLutSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssrIndirectShader,
            *inputs.ssrIndirectTarget,
            context.renderWidth,
            context.renderHeight,
            indirectClear);
        if (inputs.ssrEnabled)
        {
            outputs.indirectCompositeSrv = inputs.ssrIndirectTarget->srvCpuHandle;
        }
        outputs.ssrIndirectRan = true;
        ssrIndirectScope.Success();
    }
}
