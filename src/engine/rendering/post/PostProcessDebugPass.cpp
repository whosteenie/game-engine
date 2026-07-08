#include "engine/rendering/post/PostProcessDebugPass.h"

#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"

#include <glm/glm.hpp>

namespace
{
    bool IsPostProcessDebugMode(const RenderDebugMode mode)
    {
        return mode == RenderDebugMode::Ssao ||
               mode == RenderDebugMode::GtaoRaw ||
               mode == RenderDebugMode::GtaoFiltered ||
               mode == RenderDebugMode::CompositeOcclusion ||
               mode == RenderDebugMode::MotionVectors ||
               IsGBufferDebugMode(mode) ||
               IsRadianceDebugMode(mode) ||
               IsGiTemporalDebugMode(mode) ||
               IsSsgiDenoiseDebugMode(mode) ||
               IsSsrDebugMode(mode) ||
               IsDxrDebugMode(mode);
    }

    int SsrDebugModeIndex(const RenderDebugMode mode)
    {
        return mode == RenderDebugMode::SsrSceneValidity ? 1 : 0;
    }

    int SsrTraceDebugModeIndex(const RenderDebugMode mode)
    {
        return mode == RenderDebugMode::SsrTraceConfidence ? 1 : 0;
    }

    int SsrDenoiseDebugModeIndex(const RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::SsrDenoiseTemporal:
            return 1;
        case RenderDebugMode::SsrDenoiseFinal:
        case RenderDebugMode::SsrUpscaled:
            return 2;
        case RenderDebugMode::SsrSvgfVariance:
            return 3;
        default:
            return 0;
        }
    }

    int GiTemporalDebugModeIndex(const RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::GiDisocclusion:
            return 1;
        case RenderDebugMode::RadianceTemporalDelta:
            return 2;
        default:
            return 0;
        }
    }

    int RadianceDebugModeIndex(const RenderDebugMode mode)
    {
        return mode == RenderDebugMode::RadianceValidity ? 1 : 0;
    }

    int GBufferDebugModeIndex(const RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::GBufferAlbedo:
            return 0;
        case RenderDebugMode::GBufferRoughness:
            return 1;
        case RenderDebugMode::GBufferMetallic:
            return 2;
        case RenderDebugMode::GBufferEmissive:
            return 3;
        default:
            return 0;
        }
    }

    int SsgiDebugModeIndex(const RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::SsgiTraceHitMask:
            return 1;
        case RenderDebugMode::SsgiTraceHitDistance:
            return 2;
        default:
            return 0;
        }
    }

    void FinishDebugView(
        const PostProcessDebugPassInputs& inputs,
        PostProcessDebugPassOutputs& outputs,
        const char* ssaoDebugViewSource)
    {
        outputs.ssaoDebugViewSource = ssaoDebugViewSource;
        if (inputs.captureSsaoDiagnostics)
        {
            inputs.captureSsaoDiagnostics(
                inputs.runAo,
                inputs.compositeRan,
                inputs.compositeUsesSsao,
                inputs.pbrDebugActive,
                inputs.useShadowFactorComposite,
                inputs.hdrColorSource,
                ssaoDebugViewSource,
                inputs.hdrColorSrv,
                inputs.shadowFactorSrv);
        }
        if (inputs.logSsaoApplySnapshot)
        {
            outputs.requestGpuReadback = true;
        }
        outputs.earlyOut = true;
    }
}

bool PostProcessDebugPass::TryExecute(
    const PostProcessContext& context,
    const PostProcessDebugPassInputs& inputs,
    PostProcessDebugPassOutputs& outputs)
{
    outputs.earlyOut = false;
    outputs.requestGpuReadback = false;
    outputs.ssaoDebugViewSource = inputs.ssaoDebugViewSource;

    if (inputs.pbrDebugActive && inputs.debugChannelShader != nullptr)
    {
        inputs.debugChannelShader->Use(false, true);
        inputs.debugChannelShader->SetInt("uOutputRgb", 1);
        inputs.debugChannelShader->SetInt("uOutputAlpha", 0);
        inputs.debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
        inputs.debugChannelShader->SetInt("uInput", 0);
        inputs.debugChannelShader->BindTextureSlot(0, inputs.hdrColorSrv);
        inputs.debugChannelShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, inputs.ssaoDebugViewSource);
        return true;
    }

    if (!IsPostProcessDebugMode(inputs.debugMode))
    {
        return false;
    }

    std::uintptr_t debugSrv = 0;
    const char* ssaoDebugViewSource = inputs.ssaoDebugViewSource;

    if (inputs.debugMode == RenderDebugMode::Ssao)
    {
        debugSrv = inputs.ssaoTarget != nullptr ? inputs.ssaoTarget->srvCpuHandle : 0;
        ssaoDebugViewSource = inputs.runAo
            ? ((inputs.ssaoShaderDebugMode != 0) ? "ssao_raw_debug" : "ssao_blur_live")
            : "ssao_blur_stale_pass_off";
    }
    else if (inputs.debugMode == RenderDebugMode::GtaoRaw)
    {
        debugSrv = inputs.gtaoRawTarget != nullptr ? inputs.gtaoRawTarget->srvCpuHandle : 0;
        ssaoDebugViewSource = inputs.runGtao ? "gtao_raw" : "gtao_raw_stale_or_inactive";
    }
    else if (inputs.debugMode == RenderDebugMode::GtaoFiltered)
    {
        debugSrv = inputs.aoCompositeSrv;
        ssaoDebugViewSource = inputs.runGtao ? "gtao_filtered" : "gtao_filtered_stale_or_inactive";
    }
    else if (inputs.debugMode == RenderDebugMode::CompositeOcclusion && inputs.runAo)
    {
        debugSrv = inputs.hdrCompositeTarget != nullptr ? inputs.hdrCompositeTarget->srvCpuHandle : 0;
        ssaoDebugViewSource = "composite_occlusion";
    }
    else if (
        inputs.debugMode == RenderDebugMode::MotionVectors &&
        inputs.sceneFramebuffer != nullptr &&
        inputs.sceneFramebuffer->HasVelocity() &&
        inputs.velocityDebugShader != nullptr)
    {
        inputs.velocityDebugShader->Use(false, true);
        inputs.velocityDebugShader->SetInt("uVelocityMap", 0);
        inputs.velocityDebugShader->SetInt("uDepthMap", 1);
        inputs.velocityDebugShader->SetFloat("uVelocityScale", 80.0f);
        inputs.velocityDebugShader->BindTextureSlot(
            0, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity));
        inputs.velocityDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.velocityDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "motion_vectors");
        return true;
    }
    else if (
        IsGBufferDebugMode(inputs.debugMode) &&
        inputs.sceneFramebuffer != nullptr &&
        inputs.sceneFramebuffer->HasMaterialGbuffer() &&
        inputs.gbufferDebugShader != nullptr)
    {
        inputs.gbufferDebugShader->Use(false, true);
        inputs.gbufferDebugShader->SetInt("uMaterial0Map", 0);
        inputs.gbufferDebugShader->SetInt("uMaterial1Map", 1);
        inputs.gbufferDebugShader->SetInt("uDepthMap", 2);
        inputs.gbufferDebugShader->SetInt("uGBufferDebugMode", GBufferDebugModeIndex(inputs.debugMode));
        inputs.gbufferDebugShader->BindTextureSlot(
            0, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough));
        inputs.gbufferDebugShader->BindTextureSlot(
            1, inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic));
        inputs.gbufferDebugShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.gbufferDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "gbuffer_material");
        return true;
    }
    else if (
        IsRadianceDebugMode(inputs.debugMode) &&
        inputs.radianceTarget != nullptr &&
        inputs.radianceTarget->srvCpuHandle != 0 &&
        inputs.radianceDebugShader != nullptr &&
        inputs.sceneFramebuffer != nullptr)
    {
        inputs.radianceDebugShader->Use(false, true);
        inputs.radianceDebugShader->SetInt("uRadianceMap", 0);
        inputs.radianceDebugShader->SetInt("uDepthMap", 1);
        inputs.radianceDebugShader->SetInt(
            "uRadianceDebugMode", RadianceDebugModeIndex(inputs.debugMode));
        inputs.radianceDebugShader->BindTextureSlot(0, inputs.radianceTarget->srvCpuHandle);
        inputs.radianceDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.radianceDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "radiance_buffer");
        return true;
    }
    else if (
        IsSsrSceneDebugMode(inputs.debugMode) &&
        inputs.ssrSceneColorTarget != nullptr &&
        inputs.ssrSceneColorTarget->srvCpuHandle != 0 &&
        inputs.ssrDebugShader != nullptr &&
        inputs.sceneFramebuffer != nullptr)
    {
        inputs.ssrDebugShader->Use(false, true);
        inputs.ssrDebugShader->SetInt("uSceneColorMap", 0);
        inputs.ssrDebugShader->SetInt("uDepthMap", 1);
        inputs.ssrDebugShader->SetInt("uSsrDebugMode", SsrDebugModeIndex(inputs.debugMode));
        inputs.ssrDebugShader->BindTextureSlot(0, inputs.ssrSceneColorTarget->srvCpuHandle);
        inputs.ssrDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.ssrDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "ssr_scene_color");
        return true;
    }
    else if (
        IsSsrTraceDebugMode(inputs.debugMode) &&
        inputs.ssrTraceTarget != nullptr &&
        inputs.ssrTraceTarget->srvCpuHandle != 0 &&
        inputs.ssrTraceDebugShader != nullptr &&
        inputs.sceneFramebuffer != nullptr)
    {
        inputs.ssrTraceDebugShader->Use(false, true);
        inputs.ssrTraceDebugShader->SetInt("uTraceMap", 0);
        inputs.ssrTraceDebugShader->SetInt("uDepthMap", 1);
        inputs.ssrTraceDebugShader->SetInt(
            "uSsrTraceDebugMode",
            SsrTraceDebugModeIndex(inputs.debugMode));
        inputs.ssrTraceDebugShader->BindTextureSlot(0, inputs.ssrTraceTarget->srvCpuHandle);
        inputs.ssrTraceDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.ssrTraceDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "ssr_trace");
        return true;
    }
    else if (IsSsrDenoiseDebugMode(inputs.debugMode))
    {
        std::uintptr_t denoiseDebugSrv = 0;
        const char* debugSource = "ssr_denoise";
        const std::uintptr_t traceFallback =
            inputs.ssrTraceTarget != nullptr ? inputs.ssrTraceTarget->srvCpuHandle : 0;

        if (inputs.debugMode == RenderDebugMode::SsrDenoiseSpatial)
        {
            denoiseDebugSrv = inputs.lastSsrSpatialSrv != 0 ? inputs.lastSsrSpatialSrv : traceFallback;
            debugSource = "ssr_denoise_spatial";
        }
        else if (inputs.debugMode == RenderDebugMode::SsrDenoiseTemporal)
        {
            denoiseDebugSrv = inputs.lastSsrTemporalSrv != 0 ? inputs.lastSsrTemporalSrv : traceFallback;
            debugSource = "ssr_svgf_temporal";
        }
        else if (inputs.debugMode == RenderDebugMode::SsrSvgfVariance)
        {
            denoiseDebugSrv = inputs.lastSsrVarianceSrv != 0 ? inputs.lastSsrVarianceSrv : traceFallback;
            debugSource = "ssr_svgf_variance";
        }
        else if (inputs.debugMode == RenderDebugMode::SsrDenoiseFinal)
        {
            denoiseDebugSrv = inputs.lastSsrDenoiseSrv != 0 ? inputs.lastSsrDenoiseSrv : traceFallback;
            debugSource = "ssr_svgf_final";
        }
        else if (inputs.debugMode == RenderDebugMode::SsrUpscaled)
        {
            denoiseDebugSrv = inputs.lastSsrResolvedSrv != 0 ? inputs.lastSsrResolvedSrv : traceFallback;
            debugSource = "ssr_upscaled";
        }

        if (denoiseDebugSrv != 0 && inputs.ssrDenoiseDebugShader != nullptr && inputs.sceneFramebuffer != nullptr)
        {
            inputs.ssrDenoiseDebugShader->Use(false, true);
            inputs.ssrDenoiseDebugShader->SetInt("uTraceMap", 0);
            inputs.ssrDenoiseDebugShader->SetInt("uDepthMap", 1);
            inputs.ssrDenoiseDebugShader->SetInt(
                "uDebugMode",
                SsrDenoiseDebugModeIndex(inputs.debugMode));
            inputs.ssrDenoiseDebugShader->SetFloat(
                "uDebugScale",
                inputs.debugMode == RenderDebugMode::SsrSvgfVariance ? 8.0f : 1.0f);
            inputs.ssrDenoiseDebugShader->BindTextureSlot(0, denoiseDebugSrv);
            inputs.ssrDenoiseDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
            inputs.ssrDenoiseDebugShader->FlushUniforms();
            context.draw.DrawFullscreenQuad();
            FinishDebugView(inputs, outputs, debugSource);
            return true;
        }
    }
    else if (
        inputs.debugMode == RenderDebugMode::RtSpecReplacement &&
        inputs.runRtIndirect &&
        inputs.rtIndirectTarget != nullptr &&
        inputs.rtIndirectTarget->srvCpuHandle != 0 &&
        inputs.debugChannelShader != nullptr)
    {
        inputs.debugChannelShader->Use(false, true);
        inputs.debugChannelShader->SetInt("uOutputRgb", 1);
        inputs.debugChannelShader->SetInt("uOutputAlpha", 0);
        inputs.debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
        inputs.debugChannelShader->SetInt("uInput", 0);
        inputs.debugChannelShader->BindTextureSlot(0, inputs.rtIndirectTarget->srvCpuHandle);
        inputs.debugChannelShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        outputs.earlyOut = true;
        return true;
    }
    else if (
        IsSsrCompositeDebugMode(inputs.debugMode) &&
        inputs.runSsrIndirect &&
        inputs.ssrIndirectTarget != nullptr &&
        inputs.ssrIndirectTarget->srvCpuHandle != 0 &&
        inputs.ssrDenoiseDebugShader != nullptr &&
        inputs.sceneFramebuffer != nullptr)
    {
        inputs.ssrDenoiseDebugShader->Use(false, true);
        inputs.ssrDenoiseDebugShader->SetInt("uTraceMap", 0);
        inputs.ssrDenoiseDebugShader->SetInt("uDepthMap", 1);
        inputs.ssrDenoiseDebugShader->SetInt("uDebugMode", 0);
        inputs.ssrDenoiseDebugShader->SetFloat("uDebugScale", 1.0f);
        inputs.ssrDenoiseDebugShader->BindTextureSlot(0, inputs.ssrIndirectTarget->srvCpuHandle);
        inputs.ssrDenoiseDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.ssrDenoiseDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, "ssr_spec_replacement");
        return true;
    }
    else if (IsSsgiDenoiseDebugMode(inputs.debugMode))
    {
        std::uintptr_t ssgiDebugSrv = 0;
        const char* debugSource = "ssgi_denoise";
        const std::uintptr_t radianceSrv =
            inputs.radianceTarget != nullptr ? inputs.radianceTarget->srvCpuHandle : 0;
        const std::uintptr_t traceInputSrv =
            inputs.radianceTraceInputTarget != nullptr
                ? inputs.radianceTraceInputTarget->srvCpuHandle
                : 0;

        if (inputs.debugMode == RenderDebugMode::SsgiTraceRaw)
        {
            ssgiDebugSrv = radianceSrv;
            if (inputs.ssgiEnabled && traceInputSrv != 0)
            {
                ssgiDebugSrv = traceInputSrv;
            }
            else if (
                (inputs.ssgiDenoiseEnabled || inputs.ssgiNoiseInjectionEnabled) &&
                traceInputSrv != 0)
            {
                ssgiDebugSrv = traceInputSrv;
            }
            debugSource = "ssgi_trace_raw";
        }
        else if (
            inputs.debugMode == RenderDebugMode::SsgiTraceHitMask ||
            inputs.debugMode == RenderDebugMode::SsgiTraceHitDistance)
        {
            ssgiDebugSrv = traceInputSrv;
            debugSource = inputs.debugMode == RenderDebugMode::SsgiTraceHitMask
                ? "ssgi_trace_hit_mask"
                : "ssgi_trace_hit_distance";
        }
        else if (inputs.debugMode == RenderDebugMode::SsgiDenoiseSpatial)
        {
            ssgiDebugSrv = inputs.radianceSpatialTarget != nullptr
                ? inputs.radianceSpatialTarget->srvCpuHandle
                : 0;
            debugSource = "ssgi_denoise_spatial";
        }
        else if (
            inputs.debugMode == RenderDebugMode::SsgiDenoiseTemporal ||
            inputs.debugMode == RenderDebugMode::SsgiDenoiseFinal)
        {
            ssgiDebugSrv = inputs.radianceHistoryTarget != nullptr
                ? inputs.radianceHistoryTarget->srvCpuHandle
                : 0;
            debugSource = inputs.debugMode == RenderDebugMode::SsgiDenoiseTemporal
                ? "ssgi_denoise_temporal"
                : "ssgi_denoise_final";
        }
        else if (inputs.debugMode == RenderDebugMode::SsgiInject)
        {
            ssgiDebugSrv = inputs.lastSsgiInjectSrv != 0 ? inputs.lastSsgiInjectSrv : radianceSrv;
            debugSource = "ssgi_inject";
        }
        else if (inputs.debugMode == RenderDebugMode::SsgiFinalContribution)
        {
            ssgiDebugSrv = inputs.lastSsgiInjectSrv != 0 ? inputs.lastSsgiInjectSrv : radianceSrv;
            debugSource = "ssgi_final_contribution";
        }

        if (ssgiDebugSrv != 0 && inputs.ssgiDenoiseDebugShader != nullptr && inputs.sceneFramebuffer != nullptr)
        {
            inputs.ssgiDenoiseDebugShader->Use(false, true);
            inputs.ssgiDenoiseDebugShader->SetInt("uRadianceMap", 0);
            inputs.ssgiDenoiseDebugShader->SetInt("uDepthMap", 1);
            inputs.ssgiDenoiseDebugShader->SetInt("uDebugMode", SsgiDebugModeIndex(inputs.debugMode));
            inputs.ssgiDenoiseDebugShader->SetFloat(
                "uDebugScale",
                inputs.debugMode == RenderDebugMode::SsgiFinalContribution ? inputs.ssgiStrength : 1.0f);
            inputs.ssgiDenoiseDebugShader->BindTextureSlot(0, ssgiDebugSrv);
            inputs.ssgiDenoiseDebugShader->BindTextureSlot(1, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
            inputs.ssgiDenoiseDebugShader->FlushUniforms();
            context.draw.DrawFullscreenQuad();
            FinishDebugView(inputs, outputs, debugSource);
            return true;
        }
    }
    else if (
        IsGiTemporalDebugMode(inputs.debugMode) &&
        inputs.radianceHistoryTarget != nullptr &&
        inputs.radianceHistoryTarget->srvCpuHandle != 0 &&
        inputs.giTemporalDebugShader != nullptr &&
        inputs.sceneFramebuffer != nullptr &&
        inputs.radianceTarget != nullptr)
    {
        inputs.giTemporalDebugShader->Use(false, true);
        inputs.giTemporalDebugShader->SetInt("uTemporalRadiance", 0);
        inputs.giTemporalDebugShader->SetInt("uCurrentRadiance", 1);
        inputs.giTemporalDebugShader->SetInt("uDepthMap", 2);
        inputs.giTemporalDebugShader->SetInt(
            "uGiTemporalDebugMode",
            GiTemporalDebugModeIndex(inputs.debugMode));
        inputs.giTemporalDebugShader->SetFloat("uDifferenceGain", 25.0f);
        inputs.giTemporalDebugShader->BindTextureSlot(0, inputs.radianceHistoryTarget->srvCpuHandle);
        inputs.giTemporalDebugShader->BindTextureSlot(1, inputs.radianceTarget->srvCpuHandle);
        inputs.giTemporalDebugShader->BindTextureSlot(2, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.giTemporalDebugShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(
            inputs,
            outputs,
            inputs.debugMode == RenderDebugMode::GiDisocclusion
                ? "gi_disocclusion"
                : (inputs.debugMode == RenderDebugMode::RadianceTemporalDelta
                      ? "radiance_temporal_delta"
                      : "radiance_temporal"));
        return true;
    }

    if (debugSrv != 0 && inputs.debugChannelShader != nullptr)
    {
        inputs.debugChannelShader->Use(false, true);
        inputs.debugChannelShader->SetInt("uOutputRgb", 0);
        inputs.debugChannelShader->SetInt("uOutputAlpha", 0);
        inputs.debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
        inputs.debugChannelShader->SetInt("uInput", 0);
        inputs.debugChannelShader->BindTextureSlot(0, debugSrv);
        inputs.debugChannelShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();
        FinishDebugView(inputs, outputs, ssaoDebugViewSource);
        return true;
    }

    return false;
}
