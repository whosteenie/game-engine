#include "engine/rendering/post/ScreenSpaceEffects.h"

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>
#include <algorithm>
#include <cmath>

float ScreenSpaceEffects::GetSsaoRadius() const
{
    return m_ssaoRadius;
}

void ScreenSpaceEffects::SetSsaoRadius(float radius)
{
    m_ssaoRadius = std::max(radius, 0.01f);
}

float ScreenSpaceEffects::GetSsaoBias() const
{
    return m_ssaoBias;
}

void ScreenSpaceEffects::SetSsaoBias(float bias)
{
    m_ssaoBias = std::max(bias, 0.0f);
}

float ScreenSpaceEffects::GetSsaoPower() const
{
    return m_ssaoPower;
}

void ScreenSpaceEffects::SetSsaoPower(float power)
{
    m_ssaoPower = std::max(power, 0.1f);
}

float ScreenSpaceEffects::GetGtaoRadius() const
{
    return m_gtaoRadius;
}

void ScreenSpaceEffects::SetGtaoRadius(const float radius)
{
    m_gtaoRadius = std::clamp(radius, 0.05f, 5.0f);
}

float ScreenSpaceEffects::GetGtaoThickness() const
{
    return m_gtaoThickness;
}

void ScreenSpaceEffects::SetGtaoThickness(const float thickness)
{
    m_gtaoThickness = std::clamp(thickness, 0.02f, 2.0f);
}

float ScreenSpaceEffects::GetGtaoFalloff() const
{
    return m_gtaoFalloff;
}

void ScreenSpaceEffects::SetGtaoFalloff(const float falloff)
{
    m_gtaoFalloff = std::clamp(falloff, 0.25f, 6.0f);
}

float ScreenSpaceEffects::GetGtaoPower() const
{
    return m_gtaoPower;
}

void ScreenSpaceEffects::SetGtaoPower(const float power)
{
    m_gtaoPower = std::clamp(power, 0.25f, 4.0f);
}

int ScreenSpaceEffects::GetGtaoDirections() const
{
    return m_gtaoDirections;
}

void ScreenSpaceEffects::SetGtaoDirections(const int directions)
{
    m_gtaoDirections = std::clamp(directions, 2, 8);
}

int ScreenSpaceEffects::GetGtaoSteps() const
{
    return m_gtaoSteps;
}

void ScreenSpaceEffects::SetGtaoSteps(const int steps)
{
    m_gtaoSteps = std::clamp(steps, 2, 12);
}

bool ScreenSpaceEffects::IsGtaoDenoiseEnabled() const
{
    return m_gtaoDenoiseEnabled;
}

void ScreenSpaceEffects::SetGtaoDenoiseEnabled(const bool enabled)
{
    m_gtaoDenoiseEnabled = enabled;
}

int ScreenSpaceEffects::GetSsaoShaderDebugMode() const
{
    return m_ssaoShaderDebugMode;
}

void ScreenSpaceEffects::SetSsaoShaderDebugMode(const int mode)
{
    m_ssaoShaderDebugMode = std::clamp(mode, 0, 6);
}

float ScreenSpaceEffects::GetAoStrength() const
{
    return m_aoStrength;
}

void ScreenSpaceEffects::SetAoStrength(float strength)
{
    m_aoStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetExposure() const
{
    return m_exposure;
}

void ScreenSpaceEffects::SetExposure(float exposure)
{
    m_exposure = std::clamp(exposure, -4.0f, 6.0f);
}

TonemapMode ScreenSpaceEffects::GetTonemapMode() const
{
    return m_tonemapMode;
}

void ScreenSpaceEffects::SetTonemapMode(TonemapMode mode)
{
    m_tonemapMode = mode;
}

bool ScreenSpaceEffects::IsBloomEnabled() const
{
    return m_bloomEnabled;
}

void ScreenSpaceEffects::SetBloomEnabled(bool enabled)
{
    m_bloomEnabled = enabled;
}

float ScreenSpaceEffects::GetBloomThreshold() const
{
    return m_bloomThreshold;
}

void ScreenSpaceEffects::SetBloomThreshold(float threshold)
{
    m_bloomThreshold = std::max(threshold, 0.0f);
}

float ScreenSpaceEffects::GetBloomSoftKnee() const
{
    return m_bloomSoftKnee;
}

void ScreenSpaceEffects::SetBloomSoftKnee(float softKnee)
{
    m_bloomSoftKnee = std::clamp(softKnee, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetBloomIntensity() const
{
    return m_bloomIntensity;
}

void ScreenSpaceEffects::SetBloomIntensity(float intensity)
{
    m_bloomIntensity = std::max(intensity, 0.0f);
}

float ScreenSpaceEffects::GetBloomBlurRadius() const
{
    return m_bloomBlurRadius;
}

void ScreenSpaceEffects::SetBloomBlurRadius(float blurRadius)
{
    m_bloomBlurRadius = std::clamp(blurRadius, 0.25f, 4.0f);
}

RenderDebugMode ScreenSpaceEffects::GetDebugMode() const
{
    return m_debugMode;
}

void ScreenSpaceEffects::SetDebugMode(const RenderDebugMode mode)
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "pt-temporal-diagnostics", "diagnostic-input",
        m_pathTracerActive ? "path-tracer" : "raster", "debug-mode-selection",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight,
        false, mode != RenderDebugMode::None, static_cast<std::uint32_t>(mode));
    if (IsRtPrimaryDebugMode(mode) && !IsRtPrimaryDebugMode(m_debugMode))
    {
        m_rtPrimaryDebugSettleFrames = 0;
    }

    const bool isolateChanged =
        PtDebugIsolateModeFromRenderDebug(m_debugMode) != PtDebugIsolateModeFromRenderDebug(mode);
    const bool temporalDiagnosticsChanged =
        (IsPtTemporalStatsDebugMode(m_debugMode) || IsPtMotionReprojectionDebugMode(m_debugMode)
            || IsPtDepthReprojectionDebugMode(m_debugMode)
            || IsPtMatrixDepthReprojectionDebugMode(m_debugMode))
        != (IsPtTemporalStatsDebugMode(mode) || IsPtMotionReprojectionDebugMode(mode)
            || IsPtDepthReprojectionDebugMode(mode)
            || IsPtMatrixDepthReprojectionDebugMode(mode));
    const bool giTemporalDiagnosticKindChanged =
        IsPtRestirGiSpatialStatsDebugMode(m_debugMode)
        && IsPtRestirGiSpatialStatsDebugMode(mode)
        && m_debugMode != mode;
    m_debugMode = mode;
    if (isolateChanged || temporalDiagnosticsChanged || giTemporalDiagnosticKindChanged)
    {
        ResetPathTracerTemporalDiagnostics();
    }
}

void ScreenSpaceEffects::ResetRtPrimaryDebugBlitSettle()
{
    m_rtPrimaryDebugSettleFrames = 0;
}

void ScreenSpaceEffects::NotifyRtPrimaryDebugDispatched()
{
    if (!IsRtPrimaryDebugMode(m_debugMode))
    {
        return;
    }

    if (m_rtPrimaryDebugSettleFrames < 3)
    {
        ++m_rtPrimaryDebugSettleFrames;
    }
}

bool ScreenSpaceEffects::IsRtPrimaryDebugBlitReady() const
{
    if (!IsRtPrimaryDebugMode(m_debugMode))
    {
        return false;
    }

    return m_rtPrimaryDebugSettleFrames >= 2;
}

bool ScreenSpaceEffects::IsPathTracerBlitReady() const
{
    return m_dxrPrimaryDebugShader != nullptr;
}

void ScreenSpaceEffects::SetDxrSmokeDebugSrv(const std::uintptr_t srvCpuHandle)
{
    m_dxrSmokeDebugSrv = srvCpuHandle;
}

void ScreenSpaceEffects::SetDxrPrimaryDebugSrvs(
    const std::uintptr_t primaryOutputSrvCpuHandle,
    const std::uintptr_t primaryMetadataSrvCpuHandle)
{
    m_dxrPrimaryOutputSrv = primaryOutputSrvCpuHandle;
    m_dxrPrimaryMetadataSrv = primaryMetadataSrvCpuHandle;
}

void ScreenSpaceEffects::SetDxrReflectionSrv(
    const std::uintptr_t reflectionSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY,
    const std::uintptr_t denoisedSrvCpuHandle,
    const float maxTraceDistance)
{
    m_dxrReflectionSrv = reflectionSrvCpuHandle;
    m_dxrReflectionDenoisedSrv = denoisedSrvCpuHandle;
    m_dxrReflectionUvScaleX = uvScaleX;
    m_dxrReflectionUvScaleY = uvScaleY;
    m_dxrReflectionMaxTraceDistance = maxTraceDistance;
}

bool ScreenSpaceEffects::ReflectionCompositeReplacesSpecIbl(
    const bool dxrReflectionsEnabled, const bool iblReady, const RenderDebugMode debugMode) const
{
    // Must match the runRtIndirect / runSsrIndirect gating in Apply (minus trace freshness, which
    // is handled by the pure-IBL fallback). PBR debug modes render specific channels, not the
    // composite, so leave spec IBL baked there.
    if (IsPbrMaterialDebugMode(debugMode) || !iblReady)
    {
        return false;
    }
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->HasSplitLighting()
        || !m_sceneFramebuffer->HasMaterialGbuffer())
    {
        return false;
    }
    return dxrReflectionsEnabled || m_ssrEnabled;
}

bool ScreenSpaceEffects::GiInjectReplacesDiffuseIbl(
    const bool giActive, const bool iblReady, const RenderDebugMode debugMode) const
{
    // Must match the runRtGiInject gating in Apply (minus trace freshness, handled by the SH
    // fallback). PBR debug modes render specific channels, not the inject — leave ambient baked.
    if (IsPbrMaterialDebugMode(debugMode) || !iblReady)
    {
        return false;
    }
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->HasSplitLighting()
        || !m_sceneFramebuffer->HasMaterialGbuffer())
    {
        return false;
    }
    return giActive;
}

void ScreenSpaceEffects::SetDxrShadowSrv(
    const std::uintptr_t penumbraSrvCpuHandle,
    const std::uintptr_t denoisedSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY)
{
    m_dxrShadowPenumbraSrv = penumbraSrvCpuHandle;
    m_dxrShadowDenoisedSrv = denoisedSrvCpuHandle;
    m_dxrShadowUvScaleX = uvScaleX;
    m_dxrShadowUvScaleY = uvScaleY;
}

void ScreenSpaceEffects::SetDxrGiSrv(
    const std::uintptr_t giRawSrvCpuHandle,
    const std::uintptr_t giDenoisedSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY)
{
    m_dxrGiRawSrv = giRawSrvCpuHandle;
    m_dxrGiDenoisedSrv = giDenoisedSrvCpuHandle;
    m_dxrGiUvScaleX = uvScaleX;
    m_dxrGiUvScaleY = uvScaleY;
}

std::uintptr_t ScreenSpaceEffects::GetSceneColorSrvCpuHandle(const GBufferSlot slot) const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return 0;
    }

    return m_sceneFramebuffer->GetGBufferSrvCpuHandle(slot);
}

void ScreenSpaceEffects::PrepareSceneColorForDxrRead() const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    constexpr std::uint32_t kAllShaderRead = static_cast<std::uint32_t>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_sceneFramebuffer->TransitionDepthForDxrRead();
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::DirectLighting, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::IndirectLighting, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::ShadingNormal, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::SunShadowFactor, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::MotionVelocity, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::MaterialAlbedoRough, kAllShaderRead);
}

AntiAliasingMode ScreenSpaceEffects::GetAntiAliasingMode() const
{
    return m_antiAliasingMode;
}

void ScreenSpaceEffects::SetAntiAliasingMode(const AntiAliasingMode mode)
{
    if (mode == AntiAliasingMode::MSAA)
    {
        return;
    }

    AntiAliasingMode effectiveMode = mode;
    if ((mode == AntiAliasingMode::DLAA || mode == AntiAliasingMode::DLSS)
        && DlssContext::Get().IsReady() && !DlssContext::Get().IsDlssSupported())
    {
        EngineLog::Warn(
            "dlss",
            "DLSS is not supported on this GPU; falling back to TAA.");
        effectiveMode = AntiAliasingMode::TAA;
    }

    // TAA, DLAA and DLSS own the resolve stage and are incompatible with geometry MSAA.
    const bool ownsResolve = effectiveMode == AntiAliasingMode::TAA
        || effectiveMode == AntiAliasingMode::DLAA || effectiveMode == AntiAliasingMode::DLSS;
    if (ownsResolve && m_msaaSampleCount > 1)
    {
        m_msaaSampleCount = 1;
    }

    if (m_antiAliasingMode != effectiveMode)
    {
        m_lastAntiAliasingMode = effectiveMode;
        m_width = 0;
        m_height = 0;
    }

    m_antiAliasingMode = effectiveMode;
}

DlssPreset ScreenSpaceEffects::GetDlssPreset() const
{
    return m_dlssPreset;
}

void ScreenSpaceEffects::SetDlssPreset(const DlssPreset preset)
{
    if (m_dlssPreset == preset)
    {
        return;
    }
    m_dlssPreset = preset;
    // The internal render resolution changes with the preset. Force target reallocation; the
    // compatibility key schedules the owner-specific temporal reset at the next evaluation.
    if (m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        m_width = 0;
        m_height = 0;
    }
}

bool ScreenSpaceEffects::GetRayReconstruction() const
{
    return m_rayReconstruction;
}

void ScreenSpaceEffects::SetRayReconstruction(const bool enabled)
{
    if (m_rayReconstruction == enabled)
    {
        return;
    }
    m_rayReconstruction = enabled;
    // The compatibility key observes the feature change and schedules one reset at next use.
}

bool ScreenSpaceEffects::IsRayReconstructionActive() const
{
    if (!m_rayReconstruction)
    {
        return false;
    }
    if (m_antiAliasingMode != AntiAliasingMode::DLAA && m_antiAliasingMode != AntiAliasingMode::DLSS)
    {
        return false;
    }
    const DlssContext& dlss = DlssContext::Get();
    return dlss.IsReady() && dlss.IsRrSupported();
}

float ScreenSpaceEffects::GetDlssSharpness() const
{
    return m_dlssSharpness;
}

void ScreenSpaceEffects::SetDlssSharpness(const float sharpness)
{
    const float clamped = std::clamp(sharpness, 0.0f, 1.0f);
    if (m_dlssSharpness == clamped)
    {
        return;
    }
    m_dlssSharpness = clamped;
    if (m_antiAliasingMode == AntiAliasingMode::DLAA || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        ResetTaaHistory();
    }
}

DlssRrPreset ScreenSpaceEffects::GetRrPreset() const
{
    return m_rrPreset;
}

void ScreenSpaceEffects::SetRrPreset(const DlssRrPreset preset)
{
    if (m_rrPreset == preset)
    {
        return;
    }
    m_rrPreset = preset;
    // The compatibility key includes this model version and schedules one reset at next use.
}

int ScreenSpaceEffects::GetMsaaSampleCount() const
{
    return m_msaaSampleCount;
}

void ScreenSpaceEffects::SetMsaaSampleCount(const int sampleCount)
{
    int clampedCount = sampleCount;
    if (clampedCount <= 1)
    {
        clampedCount = 1;
    }
    else if (clampedCount != 2 && clampedCount != 4 && clampedCount != 8)
    {
        clampedCount = 4;
    }

    if (clampedCount > 1 && !GfxContext::Get().IsMsaaSampleCountSupported(clampedCount))
    {
        return;
    }

    if (clampedCount > 1
        && (m_antiAliasingMode == AntiAliasingMode::TAA
            || m_antiAliasingMode == AntiAliasingMode::DLAA
            || m_antiAliasingMode == AntiAliasingMode::DLSS))
    {
        SetAntiAliasingMode(AntiAliasingMode::None);
    }

    if (m_msaaSampleCount == clampedCount)
    {
        return;
    }

    m_msaaSampleCount = clampedCount;
    m_width = 0;
    m_height = 0;
}

bool ScreenSpaceEffects::IsMsaaPendingReload() const
{
    return m_msaaSampleCount != GfxContext::Get().GetActiveMsaaSampleCount();
}

void ScreenSpaceEffects::CopySettingsFrom(const ScreenSpaceEffects& source)
{
    m_enabled = source.m_enabled;
    m_ssaoEnabled = source.m_ssaoEnabled;
    m_aoMode = source.m_aoMode;
    m_ssaoRadius = source.m_ssaoRadius;
    m_ssaoBias = source.m_ssaoBias;
    m_ssaoPower = source.m_ssaoPower;
    m_gtaoRadius = source.m_gtaoRadius;
    m_gtaoThickness = source.m_gtaoThickness;
    m_gtaoFalloff = source.m_gtaoFalloff;
    m_gtaoPower = source.m_gtaoPower;
    m_gtaoDirections = source.m_gtaoDirections;
    m_gtaoSteps = source.m_gtaoSteps;
    m_gtaoDenoiseEnabled = source.m_gtaoDenoiseEnabled;
    m_ssaoShaderDebugMode = source.m_ssaoShaderDebugMode;
    m_aoStrength = source.m_aoStrength;
    m_exposure = source.m_exposure;
    m_tonemapMode = source.m_tonemapMode;
    m_bloomEnabled = source.m_bloomEnabled;
    m_bloomThreshold = source.m_bloomThreshold;
    m_bloomSoftKnee = source.m_bloomSoftKnee;
    m_bloomIntensity = source.m_bloomIntensity;
    m_bloomBlurRadius = source.m_bloomBlurRadius;
    m_bloomTemporalBlendFactor = source.m_bloomTemporalBlendFactor;
    m_bloomSameUvBlendFactor = source.m_bloomSameUvBlendFactor;
    m_bloomDepthThreshold = source.m_bloomDepthThreshold;
    m_antiAliasingMode = source.m_antiAliasingMode;
    m_dlssPreset = source.m_dlssPreset;
    m_rayReconstruction = source.m_rayReconstruction;
    m_dlssSharpness = source.m_dlssSharpness;
    m_rrPreset = source.m_rrPreset;
    m_forceDlssResetEveryFrame = source.m_forceDlssResetEveryFrame;
    m_useDilatedDlssMotionVectors = source.m_useDilatedDlssMotionVectors;
    m_reconstructDlssCameraMotion = source.m_reconstructDlssCameraMotion;
    m_freezeTemporalJitterDiagnostic = source.m_freezeTemporalJitterDiagnostic;
    m_msaaSampleCount = source.m_msaaSampleCount;
    m_fxaaSubpixQuality = source.m_fxaaSubpixQuality;
    m_fxaaEdgeThreshold = source.m_fxaaEdgeThreshold;
    m_renderScale = source.m_renderScale;
    m_taaBlendFactor = source.m_taaBlendFactor;
    m_giTemporalBlendFactor = source.m_giTemporalBlendFactor;
    m_giDepthThreshold = source.m_giDepthThreshold;
    m_ssgiDenoiseEnabled = source.m_ssgiDenoiseEnabled;
    m_ssgiNoiseInjectionEnabled = source.m_ssgiNoiseInjectionEnabled;
    m_ssgiNoiseStrength = source.m_ssgiNoiseStrength;
    m_ssgiSpatialDepthThreshold = source.m_ssgiSpatialDepthThreshold;
    m_ssgiSpatialBlurSpread = source.m_ssgiSpatialBlurSpread;
    m_ssgiRoughnessSpreadMin = source.m_ssgiRoughnessSpreadMin;
    m_ssgiRoughnessSpreadMax = source.m_ssgiRoughnessSpreadMax;
    m_ssgiEnabled = source.m_ssgiEnabled;
    m_ssgiStrength = source.m_ssgiStrength;
    m_ssgiMaxTraceDistance = source.m_ssgiMaxTraceDistance;
    m_ssgiStepCount = source.m_ssgiStepCount;
    m_ssgiThickness = source.m_ssgiThickness;
    m_ssrEnabled = source.m_ssrEnabled;
    m_ssrMaxTraceDistance = source.m_ssrMaxTraceDistance;
    m_ssrStepCount = source.m_ssrStepCount;
    m_ssrSampleCount = source.m_ssrSampleCount;
    m_ssrThickness = source.m_ssrThickness;
    m_ssrRoughnessCutoff = source.m_ssrRoughnessCutoff;
    m_ssrStepExponent = source.m_ssrStepExponent;
    m_ssrDenoiseEnabled = source.m_ssrDenoiseEnabled;
    m_ssrTemporalBlendFactor = source.m_ssrTemporalBlendFactor;
    m_ssrSameUvBlendFactor = source.m_ssrSameUvBlendFactor;
    m_ssrStrength = source.m_ssrStrength;
    m_ssrSpatialDepthThreshold = source.m_ssrSpatialDepthThreshold;
    m_ssrSpatialBlurSpread = source.m_ssrSpatialBlurSpread;
    m_ssrRoughnessSpreadMin = source.m_ssrRoughnessSpreadMin;
    m_ssrRoughnessSpreadMax = source.m_ssrRoughnessSpreadMax;
    m_ssrDepthThreshold = source.m_ssrDepthThreshold;
    m_smaaThreshold = source.m_smaaThreshold;
    m_smaaSearchSteps = source.m_smaaSearchSteps;
    m_ssaoBlurDepthThreshold = source.m_ssaoBlurDepthThreshold;
}

float ScreenSpaceEffects::GetRenderScale() const
{
    return m_renderScale;
}

void ScreenSpaceEffects::SetRenderScale(const float scale)
{
    const float clampedScale = std::clamp(scale, 1.0f, 2.0f);
    if (m_renderScale != clampedScale)
    {
        m_renderScale = clampedScale;
        m_width = 0;
        m_height = 0;
    }
}

float ScreenSpaceEffects::GetTaaBlendFactor() const
{
    return m_taaBlendFactor;
}

void ScreenSpaceEffects::SetTaaBlendFactor(const float factor)
{
    m_taaBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetGiTemporalBlendFactor() const
{
    return m_giTemporalBlendFactor;
}

void ScreenSpaceEffects::SetGiTemporalBlendFactor(const float factor)
{
    m_giTemporalBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetGiDepthThreshold() const
{
    return m_giDepthThreshold;
}

void ScreenSpaceEffects::SetGiDepthThreshold(const float threshold)
{
    m_giDepthThreshold = std::clamp(threshold, 0.0005f, 0.05f);
}

bool ScreenSpaceEffects::IsSsgiDenoiseEnabled() const
{
    return m_ssgiDenoiseEnabled;
}

void ScreenSpaceEffects::SetSsgiDenoiseEnabled(const bool enabled)
{
    m_ssgiDenoiseEnabled = enabled;
}

bool ScreenSpaceEffects::IsSsgiNoiseInjectionEnabled() const
{
    return m_ssgiNoiseInjectionEnabled;
}

void ScreenSpaceEffects::SetSsgiNoiseInjectionEnabled(const bool enabled)
{
    m_ssgiNoiseInjectionEnabled = enabled;
}

float ScreenSpaceEffects::GetSsgiNoiseStrength() const
{
    return m_ssgiNoiseStrength;
}

void ScreenSpaceEffects::SetSsgiNoiseStrength(const float strength)
{
    m_ssgiNoiseStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetSsgiSpatialDepthThreshold() const
{
    return m_ssgiSpatialDepthThreshold;
}

void ScreenSpaceEffects::SetSsgiSpatialDepthThreshold(const float threshold)
{
    m_ssgiSpatialDepthThreshold = std::max(0.001f, threshold);
}

float ScreenSpaceEffects::GetSsgiSpatialBlurSpread() const
{
    return m_ssgiSpatialBlurSpread;
}

void ScreenSpaceEffects::SetSsgiSpatialBlurSpread(const float spread)
{
    m_ssgiSpatialBlurSpread = std::clamp(spread, 0.25f, 4.0f);
}

float ScreenSpaceEffects::GetSsgiRoughnessSpreadMin() const
{
    return m_ssgiRoughnessSpreadMin;
}

void ScreenSpaceEffects::SetSsgiRoughnessSpreadMin(const float spread)
{
    m_ssgiRoughnessSpreadMin = std::clamp(spread, 0.1f, 2.0f);
}

float ScreenSpaceEffects::GetSsgiRoughnessSpreadMax() const
{
    return m_ssgiRoughnessSpreadMax;
}

void ScreenSpaceEffects::SetSsgiRoughnessSpreadMax(const float spread)
{
    m_ssgiRoughnessSpreadMax = std::clamp(spread, 0.5f, 4.0f);
}

bool ScreenSpaceEffects::IsSsgiEnabled() const
{
    return m_ssgiEnabled;
}

void ScreenSpaceEffects::SetSsgiEnabled(const bool enabled)
{
    m_ssgiEnabled = enabled;
}

float ScreenSpaceEffects::GetSsgiStrength() const
{
    return m_ssgiStrength;
}

void ScreenSpaceEffects::SetSsgiStrength(const float strength)
{
    m_ssgiStrength = std::clamp(strength, 0.0f, 2.0f);
}

float ScreenSpaceEffects::GetSsgiMaxTraceDistance() const
{
    return m_ssgiMaxTraceDistance;
}

void ScreenSpaceEffects::SetSsgiMaxTraceDistance(const float distance)
{
    m_ssgiMaxTraceDistance = std::clamp(distance, 0.25f, 20.0f);
}

int ScreenSpaceEffects::GetSsgiStepCount() const
{
    return m_ssgiStepCount;
}

void ScreenSpaceEffects::SetSsgiStepCount(const int steps)
{
    m_ssgiStepCount = std::clamp(steps, 4, 32);
}

float ScreenSpaceEffects::GetSsgiThickness() const
{
    return m_ssgiThickness;
}

void ScreenSpaceEffects::SetSsgiThickness(const float thickness)
{
    m_ssgiThickness = std::clamp(thickness, 0.05f, 2.0f);
}

bool ScreenSpaceEffects::IsSsrEnabled() const
{
    return m_ssrEnabled;
}

void ScreenSpaceEffects::SetSsrEnabled(const bool enabled)
{
    if (m_ssrEnabled != enabled)
    {
        InvalidateSsrHistory();
    }
    m_ssrEnabled = enabled;
}

void ScreenSpaceEffects::InvalidateSsrHistory()
{
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
}

float ScreenSpaceEffects::GetSsrMaxTraceDistance() const
{
    return m_ssrMaxTraceDistance;
}

void ScreenSpaceEffects::SetSsrMaxTraceDistance(const float distance)
{
    m_ssrMaxTraceDistance = std::clamp(distance, 1.0f, 50.0f);
}

int ScreenSpaceEffects::GetSsrStepCount() const
{
    return m_ssrStepCount;
}

void ScreenSpaceEffects::SetSsrStepCount(const int steps)
{
    m_ssrStepCount = std::clamp(steps, 4, 64);
}

int ScreenSpaceEffects::GetSsrSampleCount() const
{
    return m_ssrSampleCount;
}

void ScreenSpaceEffects::SetSsrSampleCount(const int samples)
{
    m_ssrSampleCount = std::clamp(samples, 1, 8);
}

float ScreenSpaceEffects::GetSsrThickness() const
{
    return m_ssrThickness;
}

void ScreenSpaceEffects::SetSsrThickness(const float thickness)
{
    m_ssrThickness = std::clamp(thickness, 0.05f, 2.0f);
}

float ScreenSpaceEffects::GetSsrRoughnessCutoff() const
{
    return m_ssrRoughnessCutoff;
}

void ScreenSpaceEffects::SetSsrRoughnessCutoff(const float cutoff)
{
    m_ssrRoughnessCutoff = std::clamp(cutoff, 0.1f, 1.0f);
}

bool ScreenSpaceEffects::IsSsrDenoiseEnabled() const
{
    return m_ssrDenoiseEnabled;
}

void ScreenSpaceEffects::SetSsrDenoiseEnabled(const bool enabled)
{
    m_ssrDenoiseEnabled = enabled;
}

float ScreenSpaceEffects::GetSsrTemporalBlendFactor() const
{
    return m_ssrTemporalBlendFactor;
}

void ScreenSpaceEffects::SetSsrTemporalBlendFactor(const float factor)
{
    m_ssrTemporalBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetSsrStrength() const
{
    return m_ssrStrength;
}

void ScreenSpaceEffects::SetSsrStrength(const float strength)
{
    m_ssrStrength = std::clamp(strength, 0.0f, 2.0f);
}

bool ScreenSpaceEffects::GetSsrSceneColorRanLastFrame() const
{
    return m_ssrSceneColorRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrTraceRanLastFrame() const
{
    return m_ssrTraceRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrDenoiseRanLastFrame() const
{
    return m_ssrDenoiseRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrTemporalRanLastFrame() const
{
    return m_ssrTemporalRanLastFrame;
}

int ScreenSpaceEffects::GetSsrTraceTargetWidth() const
{
    return m_ssrTraceTarget.width;
}

int ScreenSpaceEffects::GetSsrTraceTargetHeight() const
{
    return m_ssrTraceTarget.height;
}

float ScreenSpaceEffects::GetSmaaThreshold() const
{
    return m_smaaThreshold;
}

void ScreenSpaceEffects::SetSmaaThreshold(const float threshold)
{
    m_smaaThreshold = std::clamp(threshold, 0.01f, 0.25f);
}

int ScreenSpaceEffects::GetSmaaSearchSteps() const
{
    return m_smaaSearchSteps;
}

void ScreenSpaceEffects::SetSmaaSearchSteps(const int steps)
{
    m_smaaSearchSteps = std::clamp(steps, 1, 8);
}

float ScreenSpaceEffects::GetFxaaSubpixQuality() const
{
    return m_fxaaSubpixQuality;
}

void ScreenSpaceEffects::SetFxaaSubpixQuality(const float quality)
{
    m_fxaaSubpixQuality = std::clamp(quality, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetFxaaEdgeThreshold() const
{
    return m_fxaaEdgeThreshold;
}

void ScreenSpaceEffects::SetFxaaEdgeThreshold(const float threshold)
{
    m_fxaaEdgeThreshold = std::clamp(threshold, 0.03125f, 0.5f);
}

float ScreenSpaceEffects::GetSsaoBlurDepthThreshold() const
{
    return m_ssaoBlurDepthThreshold;
}

void ScreenSpaceEffects::SetSsaoBlurDepthThreshold(const float threshold)
{
    m_ssaoBlurDepthThreshold = std::clamp(threshold, 0.001f, 0.25f);
}

std::uintptr_t ScreenSpaceEffects::GetSceneDepthSrvCpuHandle() const
{
    if (m_sceneFramebuffer == nullptr)
    {
        return 0;
    }

    return m_sceneFramebuffer->GetDepthSrvCpuHandle();
}

bool ScreenSpaceEffects::BlitDepthToFramebuffer(const Framebuffer* viewportTarget) const
{
    if (viewportTarget == nullptr || !m_enabled || m_sceneFramebuffer == nullptr
        || !m_sceneFramebuffer->IsValid() || m_sceneFramebuffer->GetDepthResource() == nullptr
        || viewportTarget->GetDepthResource() == nullptr)
    {
        return false;
    }

    return viewportTarget->CopyDepthFrom(*m_sceneFramebuffer);
}
