#include "engine/rendering/post/ScreenSpaceGiPass.h"

#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

void ScreenSpaceGiPass::Execute(
    const PostProcessContext& context,
    const ScreenSpaceGiPassInputs& inputs,
    ScreenSpaceGiPassOutputs& outputs)
{
    outputs.giFrameIndex = inputs.giFrameIndex;
    outputs.radianceHistoryValid = inputs.radianceHistoryValid;
    outputs.lastSsgiInjectSrv = 0;

    if (!inputs.runRadianceAssembly)
    {
        return;
    }

    const float radianceClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uintptr_t temporalInputSrv = inputs.radianceSrv;

    const bool runSsgiTrace =
        inputs.ssgiEnabled &&
        inputs.sceneHasGeometryNormals &&
        inputs.radianceTraceInputTarget != nullptr &&
        inputs.radianceTraceInputTarget->resource != nullptr;

    outputs.runSsgiTrace = runSsgiTrace;

    if (runSsgiTrace)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope traceScope("ssgi trace");
        inputs.ssgiTraceShader->Use(false);
        inputs.ssgiTraceShader->SetInt("uDepthMap", 0);
        inputs.ssgiTraceShader->SetInt("uNormalMap", 1);
        inputs.ssgiTraceShader->SetInt("uMaterial0Map", 2);
        inputs.ssgiTraceShader->SetInt("uMaterial1Map", 3);
        inputs.ssgiTraceShader->SetInt("uRadianceMap", 4);
        inputs.ssgiTraceShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssgiTraceShader->SetMat4("uProjection", inputs.projectionMatrix);
        inputs.ssgiTraceShader->SetMat4("uView", inputs.viewMatrix);
        inputs.ssgiTraceShader->SetFloat("uMaxTraceDistance", inputs.ssgiMaxTraceDistance);
        inputs.ssgiTraceShader->SetInt("uStepCount", inputs.ssgiStepCount);
        inputs.ssgiTraceShader->SetFloat("uThickness", inputs.ssgiThickness);
        inputs.ssgiTraceShader->SetFloat("uFrameIndex", static_cast<float>(outputs.giFrameIndex));
        inputs.ssgiTraceShader->SetFloat("uEdgeFadeScale", 20.0f);
        inputs.ssgiTraceShader->BindTextureSlot(0, inputs.depthSrv);
        inputs.ssgiTraceShader->BindTextureSlot(1, inputs.normalSrv);
        inputs.ssgiTraceShader->BindTextureSlot(2, inputs.material0Srv);
        inputs.ssgiTraceShader->BindTextureSlot(3, inputs.material1Srv);
        inputs.ssgiTraceShader->BindTextureSlot(4, inputs.radianceSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssgiTraceShader,
            *inputs.radianceTraceInputTarget,
            context.renderWidth,
            context.renderHeight,
            radianceClear);
        temporalInputSrv = inputs.radianceTraceInputTarget->srvCpuHandle;
        traceScope.Success();
        ssgiSection.Success();
    }

    const bool runSsgiDenoise =
        inputs.sceneHasGeometryNormals &&
        inputs.sceneHasMaterialGbuffer &&
        inputs.radianceTraceInputTarget != nullptr &&
        inputs.radianceTraceInputTarget->resource != nullptr &&
        inputs.radianceSpatialTarget != nullptr &&
        inputs.radianceSpatialTarget->resource != nullptr &&
        inputs.ssgiDenoiseEnabled &&
        (runSsgiTrace || inputs.ssgiNoiseInjectionEnabled);

    outputs.runSsgiDenoise = runSsgiDenoise;

    if (!runSsgiTrace && inputs.ssgiNoiseInjectionEnabled)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope noiseScope("ssgi noise inject");
        const float noiseStrength =
            inputs.ssgiNoiseInjectionEnabled ? inputs.ssgiNoiseStrength : 0.0f;
        inputs.ssgiNoiseInjectShader->Use(false);
        inputs.ssgiNoiseInjectShader->SetInt("uRadianceMap", 0);
        inputs.ssgiNoiseInjectShader->SetInt("uDepthMap", 1);
        inputs.ssgiNoiseInjectShader->SetFloat("uNoiseStrength", noiseStrength);
        inputs.ssgiNoiseInjectShader->SetFloat(
            "uFrameIndex",
            static_cast<float>(outputs.giFrameIndex));
        inputs.ssgiNoiseInjectShader->BindTextureSlot(0, inputs.radianceSrv);
        inputs.ssgiNoiseInjectShader->BindTextureSlot(1, inputs.depthSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.ssgiNoiseInjectShader,
            *inputs.radianceTraceInputTarget,
            context.renderWidth,
            context.renderHeight,
            radianceClear);
        temporalInputSrv = inputs.radianceTraceInputTarget->srvCpuHandle;
        noiseScope.Success();
        ssgiSection.Success();
    }

    if (runSsgiDenoise)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope denoiseScope("ssgi denoise spatial");
        inputs.ssgiDenoiseSpatialShader->Use(false);
        inputs.ssgiDenoiseSpatialShader->SetInt("uInput", 0);
        inputs.ssgiDenoiseSpatialShader->SetInt("uDepthMap", 1);
        inputs.ssgiDenoiseSpatialShader->SetInt("uNormalMap", 2);
        inputs.ssgiDenoiseSpatialShader->SetInt("uMaterial0Map", 3);
        inputs.ssgiDenoiseSpatialShader->SetMat4("uInvProjection", inputs.inverseProjectionMatrix);
        inputs.ssgiDenoiseSpatialShader->SetVec2("uTexelSize", inputs.texelSize);
        inputs.ssgiDenoiseSpatialShader->SetFloat("uDepthThreshold", inputs.ssgiSpatialDepthThreshold);
        inputs.ssgiDenoiseSpatialShader->SetFloat("uBlurSpread", inputs.ssgiSpatialBlurSpread);
        inputs.ssgiDenoiseSpatialShader->SetFloat("uRoughnessSpreadMin", inputs.ssgiRoughnessSpreadMin);
        inputs.ssgiDenoiseSpatialShader->SetFloat("uRoughnessSpreadMax", inputs.ssgiRoughnessSpreadMax);
        inputs.ssgiDenoiseSpatialShader->SetFloat("uNormalPower", 4.0f);
        static constexpr float kSsgiAtrousStepScales[] = {1.0f, 2.0f, 4.0f, 2.0f};
        std::uintptr_t atrousInputSrv = temporalInputSrv;
        bool writeToBlurTarget = true;
        for (const float stepScale : kSsgiAtrousStepScales)
        {
            PostProcessTarget& outputTarget = writeToBlurTarget
                ? *inputs.radianceSpatialBlurTarget
                : *inputs.radianceSpatialTarget;
            inputs.ssgiDenoiseSpatialShader->SetFloat("uStepScale", stepScale);
            inputs.ssgiDenoiseSpatialShader->BindTextureSlot(0, atrousInputSrv);
            inputs.ssgiDenoiseSpatialShader->BindTextureSlot(1, inputs.depthSrv);
            inputs.ssgiDenoiseSpatialShader->BindTextureSlot(2, inputs.normalSrv);
            inputs.ssgiDenoiseSpatialShader->BindTextureSlot(3, inputs.material0Srv);
            context.draw.DrawFullscreenToTarget(
                *inputs.ssgiDenoiseSpatialShader,
                outputTarget,
                context.renderWidth,
                context.renderHeight,
                radianceClear);
            atrousInputSrv = outputTarget.srvCpuHandle;
            writeToBlurTarget = !writeToBlurTarget;
        }

        temporalInputSrv = atrousInputSrv;
        denoiseScope.Success();
        ssgiSection.Success();
    }

    const bool runGiTemporal =
        inputs.radianceHistoryTarget != nullptr &&
        inputs.radianceHistoryTarget->resource != nullptr &&
        inputs.radianceTemporalTarget != nullptr &&
        inputs.radianceTemporalTarget->resource != nullptr &&
        inputs.radianceHistoryDepthTarget != nullptr &&
        inputs.radianceHistoryDepthTarget->resource != nullptr &&
        (runSsgiDenoise ||
         (!inputs.ssgiEnabled && !inputs.ssgiNoiseInjectionEnabled && !inputs.ssgiDenoiseEnabled));

    outputs.runGiTemporal = runGiTemporal;

    if (runGiTemporal)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope temporalScope("gi temporal reproject");
        const glm::mat4 unjitteredProjection = inputs.unjitteredProjectionMatrix;
        const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * inputs.viewMatrix);
        const glm::mat4 prevViewProj = inputs.motionVectorState.historyValid
            ? inputs.motionVectorState.prevViewProjection
            : unjitteredProjection * inputs.viewMatrix;
        const float temporalClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (inputs.sceneFramebuffer != nullptr)
        {
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
            commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            inputs.sceneFramebuffer->RestoreDepthShaderResource();
        }

        inputs.temporalReprojectShader->Use(false);
        inputs.temporalReprojectShader->SetInt("uCurrentRadiance", 0);
        inputs.temporalReprojectShader->SetInt("uHistoryRadiance", 1);
        inputs.temporalReprojectShader->SetInt("uDepth", 2);
        inputs.temporalReprojectShader->SetInt("uHistoryDepth", 3);
        inputs.temporalReprojectShader->SetMat4("uInvViewProj", invViewProjCurr);
        inputs.temporalReprojectShader->SetMat4("uPrevViewProj", prevViewProj);
        inputs.temporalReprojectShader->SetFloat("uBlendFactor", inputs.giTemporalBlendFactor);
        inputs.temporalReprojectShader->SetFloat(
            "uHistoryValid",
            outputs.radianceHistoryValid && inputs.motionVectorState.historyValid ? 1.0f : 0.0f);
        inputs.temporalReprojectShader->SetFloat("uTexelSizeX", inputs.texelSize.x);
        inputs.temporalReprojectShader->SetFloat("uTexelSizeY", inputs.texelSize.y);
        inputs.temporalReprojectShader->SetFloat("uDepthRejectThreshold", inputs.giDepthThreshold);
        inputs.temporalReprojectShader->BindTextureSlot(0, temporalInputSrv);
        inputs.temporalReprojectShader->BindTextureSlot(1, inputs.radianceHistoryTarget->srvCpuHandle);
        inputs.temporalReprojectShader->BindTextureSlot(2, inputs.depthSrv);
        inputs.temporalReprojectShader->BindTextureSlot(3, inputs.radianceHistoryDepthTarget->srvCpuHandle);
        context.draw.DrawFullscreenToTarget(
            *inputs.temporalReprojectShader,
            *inputs.radianceTemporalTarget,
            context.renderWidth,
            context.renderHeight,
            temporalClear);

        inputs.giDepthHistoryShader->Use(false);
        inputs.giDepthHistoryShader->SetInt("uDepth", 0);
        inputs.giDepthHistoryShader->BindTextureSlot(0, inputs.depthSrv);
        context.draw.DrawFullscreenToTarget(
            *inputs.giDepthHistoryShader,
            *inputs.radianceHistoryDepthTarget,
            context.renderWidth,
            context.renderHeight,
            temporalClear);

        std::swap(*inputs.radianceHistoryTarget, *inputs.radianceTemporalTarget);
        outputs.radianceHistoryValid = true;
        ++outputs.giFrameIndex;
        temporalScope.Success();
        ssgiSection.Success();
    }

    if (runSsgiTrace)
    {
        if (runGiTemporal && inputs.radianceHistoryTarget != nullptr
            && inputs.radianceHistoryTarget->srvCpuHandle != 0)
        {
            outputs.lastSsgiInjectSrv = inputs.radianceHistoryTarget->srvCpuHandle;
        }
        else if (runSsgiDenoise && inputs.radianceSpatialTarget != nullptr
                 && inputs.radianceSpatialTarget->srvCpuHandle != 0)
        {
            outputs.lastSsgiInjectSrv = inputs.radianceSpatialTarget->srvCpuHandle;
        }
        else if (inputs.radianceTraceInputTarget != nullptr
                 && inputs.radianceTraceInputTarget->srvCpuHandle != 0)
        {
            outputs.lastSsgiInjectSrv = inputs.radianceTraceInputTarget->srvCpuHandle;
        }
    }
}
