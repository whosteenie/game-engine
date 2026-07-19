#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

enum class DxrReflectionsQuality
{
    Low,
    Medium,
    High,
};

// Top-level rendering mode (devdoc/dxr/path-tracing.md). Hybrid = the raster + hybrid-RT pipeline
// (default, always retained). PathTraced = the unified path tracer owns the image. Mutually
// exclusive; selecting PathTraced skips CSM, scene raster, hybrid RT dispatches, and the hybrid
// lighting post chain (AO/SSR/SSGI/composite/TAA), then presents PT via integrate/DLSS/bloom/tonemap.
enum class RenderingMode
{
    Hybrid,
    PathTraced,
};

// Path-tracer convergence sub-mode (devdoc/dxr/path-tracing.md P3/P4).
// RealTime = single-frame noisy output (DLSS-RR in P4). Reference = progressive accumulation.
enum class PtConvergenceMode
{
    RealTime,
    Reference,
};

// Session-only P7 diagnostic override. Production obeys the persisted P7 toggle; the remaining
// modes split the pre-spatial boiling filter from reservoir reuse for matched automated captures.
enum class RestirGiSpatialDiagnosticMode
{
    Production,
    Baseline,
    FilterOnly,
    SpatialOnly,
    Full,
};

class DxrSettings
{
public:
    static const char* ReflectionsQualityToString(DxrReflectionsQuality quality);
    static DxrReflectionsQuality ReflectionsQualityFromString(const std::string& value);
    static const char* RenderingModeToString(RenderingMode mode);
    static RenderingMode RenderingModeFromString(const std::string& value);
    static const char* PtConvergenceModeToString(PtConvergenceMode mode);
    static PtConvergenceMode PtConvergenceModeFromString(const std::string& value);

    // Phase P0 — path tracing (devdoc/dxr/path-tracing.md). Requires master RT enabled + DXR support.
    RenderingMode GetRenderingMode() const { return m_renderingMode; }
    void SetRenderingMode(const RenderingMode mode) { m_renderingMode = mode; }
    bool IsPathTracingActive() const { return m_enabled && m_renderingMode == RenderingMode::PathTraced; }

    PtConvergenceMode GetPtConvergenceMode() const { return m_ptConvergenceMode; }
    void SetPtConvergenceMode(const PtConvergenceMode mode) { m_ptConvergenceMode = mode; }
    bool IsPtReferenceConvergence() const
    {
        return IsPathTracingActive() && m_ptConvergenceMode == PtConvergenceMode::Reference;
    }

    int GetPtMaxBounces() const { return m_ptMaxBounces; }
    void SetPtMaxBounces(const int bounces);

    bool IsPtRussianRouletteEnabled() const { return m_ptRussianRoulette; }
    void SetPtRussianRouletteEnabled(const bool enabled) { m_ptRussianRoulette = enabled; }

    bool IsPtFireflyClampEnabled() const { return m_ptFireflyClamp; }
    void SetPtFireflyClampEnabled(const bool enabled) { m_ptFireflyClamp = enabled; }

    // Real-time smooth dielectrics: trace both Fresnel lobes at the primary interface instead of
    // randomly selecting reflection or transmission every frame.  This removes branch flicker at
    // the cost of a second optical tail for each supported smooth glass pixel.
    bool IsPtDeterministicOpticalSplitEnabled() const { return m_ptDeterministicOpticalSplit; }
    void SetPtDeterministicOpticalSplitEnabled(const bool enabled)
    {
        m_ptDeterministicOpticalSplit = enabled;
    }

    // Reconstruct the owned primary-transmission signal with a second RR evaluation. Disabling
    // this is a quality/performance comparison: the full PT signal uses the primary RR history.
    bool IsPtIndependentOpticalRrLayersEnabled() const { return m_ptIndependentOpticalRrLayers; }
    void SetPtIndependentOpticalRrLayersEnabled(const bool enabled)
    {
        m_ptIndependentOpticalRrLayers = enabled;
    }

    // Replays previous-frame camera -> optical surface -> receiver paths to solve virtual motion.
    // Disabling this retains the current-frame receiver guides but uses their cheaper direct motion.
    bool IsPtOpticalMotionReplayEnabled() const { return m_ptOpticalMotionReplay; }
    void SetPtOpticalMotionReplayEnabled(const bool enabled)
    {
        m_ptOpticalMotionReplay = enabled;
    }

    // Resolves a static exact-delta mirror prefix as a primary-surface replacement domain.
    bool IsPtMirrorChainPsrEnabled() const { return m_ptMirrorChainPsr; }
    void SetPtMirrorChainPsrEnabled(const bool enabled)
    {
        m_ptMirrorChainPsr = enabled;
    }
    int GetPtPsrMaxBounces() const { return m_ptPsrMaxBounces; }
    void SetPtPsrMaxBounces(int bounces);
    float GetPtPsrSubpixelThreshold() const { return m_ptPsrSubpixelThreshold; }
    void SetPtPsrSubpixelThreshold(float threshold);

    // Real-time PT diffuse-sky SH ambient strength (devdoc/dxr/pt/crevice-darkening.md). Reference ignores.
    float GetPtAmbientStrength() const { return m_ptAmbientStrength; }
    void SetPtAmbientStrength(const float strength);

    // Cosine AO rays at the primary hit for SH ambient occlusion (0 = off). Range [0; 8].
    int GetPtAmbientAoRayCount() const { return m_ptAmbientAoRayCount; }
    void SetPtAmbientAoRayCount(const int rays);

    // Diagnostic switchboard for the P4b shimmer hunt (devdoc/dxr/pt/gi-shimmer.md): which
    // DLSS-RR inputs come from the path tracer vs the raster G-buffer in real-time PT mode.
    // 0 = Full PT (depth + PT motion + guides), 1 = Raster bundle (stable fallback),
    // 2 = PT guides only, 3 = PT depth + PT motion (diagnostic), 4 = PT depth only,
    // 5 = PT motion only (diagnostic). Modes 2-5 are intentionally mixed bundles.
    int GetPtRrBundleMode() const { return m_ptRrBundleMode; }
    void SetPtRrBundleMode(const int mode)
    {
        m_ptRrBundleMode = mode < 0 ? 0 : (mode > 5 ? 5 : mode);
    }

    // ReSTIR DI initial sampling (restir-production-roadmap.md P2): per-category candidate count for
    // bounce-0 emissive + environment direct lighting. 0 = off (plain NEE). 1 = one candidate each,
    // byte-exact with off (A/B parity anchor). N>1 = RIS over N candidates, one shadow ray each.
    int GetRestirDiCandidateCount() const { return m_restirDiCandidateCount; }
    void SetRestirDiCandidateCount(const int count)
    {
        m_restirDiCandidateCount = count < 0 ? 0 : (count > 32 ? 32 : count);
    }
    bool IsRestirDiTemporalEnabled() const { return m_restirDiTemporalEnabled; }
    void SetRestirDiTemporalEnabled(bool enabled) { m_restirDiTemporalEnabled = enabled; }
    bool IsRestirGiInitialEnabled() const { return m_restirGiInitialEnabled; }
    void SetRestirGiInitialEnabled(bool enabled) { m_restirGiInitialEnabled = enabled; }
    bool IsRestirGiTemporalEnabled() const { return m_restirGiTemporalEnabled; }
    void SetRestirGiTemporalEnabled(bool enabled) { m_restirGiTemporalEnabled = enabled; }
    bool IsRestirGiSpatialEnabled() const { return m_restirGiSpatialEnabled; }
    void SetRestirGiSpatialEnabled(bool enabled) { m_restirGiSpatialEnabled = enabled; }
    RestirGiSpatialDiagnosticMode GetRestirGiSpatialDiagnosticMode() const
    {
        return m_restirGiSpatialDiagnosticMode;
    }
    void SetRestirGiSpatialDiagnosticMode(const RestirGiSpatialDiagnosticMode mode)
    {
        m_restirGiSpatialDiagnosticMode = mode;
    }
    int GetRestirGiDiagnosticOrbitRevolutions() const
    {
        return m_restirGiDiagnosticOrbitRevolutions;
    }
    void SetRestirGiDiagnosticOrbitRevolutions(const int revolutions)
    {
        m_restirGiDiagnosticOrbitRevolutions = revolutions < 1
            ? 1 : (revolutions > 20 ? 20 : revolutions);
    }

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(const bool enabled) { m_enabled = enabled; }

    bool IsReflectionsEnabled() const { return m_reflectionsEnabled; }
    void SetReflectionsEnabled(const bool enabled) { m_reflectionsEnabled = enabled; }

    DxrReflectionsQuality GetReflectionsQuality() const { return m_reflectionsQuality; }
    void SetReflectionsQuality(const DxrReflectionsQuality quality) { m_reflectionsQuality = quality; }

    int GetReflectionsSamplesPerPixel() const { return m_reflectionsSamplesPerPixel; }
    void SetReflectionsSamplesPerPixel(const int samples);

    float GetMaxTraceDistance() const { return m_maxTraceDistance; }
    void SetMaxTraceDistance(const float distance);

    bool IsDenoiseEnabled() const { return m_denoiseEnabled; }
    void SetDenoiseEnabled(const bool enabled) { m_denoiseEnabled = enabled; }

    bool IsDebugTraceEnabled() const { return m_debugTraceEnabled; }
    void SetDebugTraceEnabled(const bool enabled) { m_debugTraceEnabled = enabled; }

    float GetTemporalBlend() const { return m_temporalBlend; }
    void SetTemporalBlend(const float blend);

    // NRD RELAX spatial A-trous iterations — higher = smoother reflections / fewer edge
    // speckles at the cost of detail (NRD range [2; 8]).
    int GetReflectionAtrousIterations() const { return m_reflectionAtrousIterations; }
    void SetReflectionAtrousIterations(const int iterations);

    // NRD anti-firefly spatial pass — suppresses isolated bright specular samples at edges.
    bool IsReflectionAntiFireflyEnabled() const { return m_reflectionAntiFirefly; }
    void SetReflectionAntiFireflyEnabled(const bool enabled) { m_reflectionAntiFirefly = enabled; }

    // Ambient-occlusion rays traced at each reflection hit (contact darkening in reflections).
    // 0 = off; higher = cleaner reflected contact shadows at higher trace cost (range [0; 16]).
    int GetReflectionAoRays() const { return m_reflectionAoRays; }
    void SetReflectionAoRays(const int rays);

    // Roughness cutoff for RT reflections. Surfaces rougher than this skip the scattered
    // reflection trace (a diffuse-wide, noisy blob the denoiser smears anyway) and fall back to
    // prefiltered-env IBL specular; the composite fades RT->IBL over the same cutoff. Diffuse GI
    // still covers their indirect. Saves trace cost on rough materials. Range [0; 1].
    float GetReflectionRoughnessCutoff() const { return m_reflectionRoughnessCutoff; }
    void SetReflectionRoughnessCutoff(const float cutoff);

    // Phase D8 — RT soft directional (sun) shadows (devdoc/dxr/shadows.md). Supplemental quality
    // tier over CSM; replaces the CSM shadow factor at composite time when enabled.
    bool IsShadowsEnabled() const { return m_shadowsEnabled; }
    void SetShadowsEnabled(const bool enabled) { m_shadowsEnabled = enabled; }

    // Sun angular radius in degrees — drives penumbra width (0.27 ~ real sun). Range 0.05-2.0.
    float GetSunAngularRadiusDegrees() const { return m_sunAngularRadiusDegrees; }
    void SetSunAngularRadiusDegrees(float degrees);

    bool IsShadowDenoiseEnabled() const { return m_shadowDenoiseEnabled; }
    void SetShadowDenoiseEnabled(const bool enabled) { m_shadowDenoiseEnabled = enabled; }

    // Phase D9 — one-bounce RT diffuse GI (devdoc/dxr/diffuse-gi.md). Adds ray-traced diffuse
    // bounce light into RT1 (indirect); mutually exclusive with SSGI inject.
    bool IsGiEnabled() const { return m_giEnabled; }
    void SetGiEnabled(const bool enabled) { m_giEnabled = enabled; }

    // Inject strength multiplier on the diffuse bounce contribution. Range 0-2.
    float GetGiStrength() const { return m_giStrength; }
    void SetGiStrength(float strength);

    bool IsGiDenoiseEnabled() const { return m_giDenoiseEnabled; }
    void SetGiDenoiseEnabled(const bool enabled) { m_giDenoiseEnabled = enabled; }

    void CopySettingsFrom(const DxrSettings& source);
    void ClampToHardwareCapabilities(bool raytracingSupported);

    nlohmann::json ToJson() const;
    void ApplyFromJson(const nlohmann::json& value);
    void ClampToHardwareWithLogging(bool raytracingSupported);

private:
    bool m_enabled = false;
    RenderingMode m_renderingMode = RenderingMode::Hybrid;
    PtConvergenceMode m_ptConvergenceMode = PtConvergenceMode::RealTime;
    int m_ptMaxBounces = 4;
    bool m_ptRussianRoulette = true;
    bool m_ptFireflyClamp = true;
    bool m_ptDeterministicOpticalSplit = false;
    bool m_ptIndependentOpticalRrLayers = true;
    bool m_ptOpticalMotionReplay = false;
    bool m_ptMirrorChainPsr = false;
    int m_ptPsrMaxBounces = 24;
    float m_ptPsrSubpixelThreshold = 0.5f;
    float m_ptAmbientStrength = 1.0f;
    int m_ptAmbientAoRayCount = 0;
    int m_ptRrBundleMode = 0;
    int m_restirDiCandidateCount = 0;
    bool m_restirDiTemporalEnabled = false;
    bool m_restirGiInitialEnabled = false;
    bool m_restirGiTemporalEnabled = false;
    bool m_restirGiSpatialEnabled = false;
    RestirGiSpatialDiagnosticMode m_restirGiSpatialDiagnosticMode =
        RestirGiSpatialDiagnosticMode::Production;
    int m_restirGiDiagnosticOrbitRevolutions = 13;
    bool m_reflectionsEnabled = false;
    DxrReflectionsQuality m_reflectionsQuality = DxrReflectionsQuality::Medium;
    int m_reflectionsSamplesPerPixel = 1;
    float m_maxTraceDistance = 100.0f;
    bool m_denoiseEnabled = true;
    bool m_debugTraceEnabled = false;
    float m_temporalBlend = 0.95f;
    int m_reflectionAtrousIterations = 5;
    bool m_reflectionAntiFirefly = true;
    int m_reflectionAoRays = 4;
    float m_reflectionRoughnessCutoff = 0.6f;
    bool m_shadowsEnabled = false;
    float m_sunAngularRadiusDegrees = 0.27f;
    bool m_shadowDenoiseEnabled = true;
    bool m_giEnabled = false;
    float m_giStrength = 1.0f;
    bool m_giDenoiseEnabled = true;
};
