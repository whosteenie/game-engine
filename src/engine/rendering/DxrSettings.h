#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

enum class DxrReflectionsQuality
{
    Low,
    Medium,
    High,
};

// Top-level rendering mode (devdoc/dxr-path-tracing.md). Hybrid = the raster + hybrid-RT pipeline
// (default, always retained). PathTraced = the unified path tracer owns the image. Mutually
// exclusive; selecting PathTraced bypasses the hybrid RT effect passes and shows the PT output.
enum class RenderingMode
{
    Hybrid,
    PathTraced,
};

// Path-tracer convergence sub-mode (devdoc/dxr-path-tracing.md P3/P4).
// RealTime = single-frame noisy output (DLSS-RR in P4). Reference = progressive accumulation.
enum class PtConvergenceMode
{
    RealTime,
    Reference,
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

    // Phase P0 — path tracing (devdoc/dxr-path-tracing.md). Requires master RT enabled + DXR support.
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

    // Real-time PT diffuse-sky SH ambient strength (devdoc/dxr-pt-crevice-darkening.md). Reference ignores.
    float GetPtAmbientStrength() const { return m_ptAmbientStrength; }
    void SetPtAmbientStrength(const float strength);

    // Cosine AO rays at the primary hit for SH ambient occlusion (0 = off). Range [0; 8].
    int GetPtAmbientAoRayCount() const { return m_ptAmbientAoRayCount; }
    void SetPtAmbientAoRayCount(const int rays);

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

    // Phase D8 — RT soft directional (sun) shadows (devdoc/dxr-shadows.md). Supplemental quality
    // tier over CSM; replaces the CSM shadow factor at composite time when enabled.
    bool IsShadowsEnabled() const { return m_shadowsEnabled; }
    void SetShadowsEnabled(const bool enabled) { m_shadowsEnabled = enabled; }

    // Sun angular radius in degrees — drives penumbra width (0.27 ~ real sun). Range 0.05-2.0.
    float GetSunAngularRadiusDegrees() const { return m_sunAngularRadiusDegrees; }
    void SetSunAngularRadiusDegrees(float degrees);

    bool IsShadowDenoiseEnabled() const { return m_shadowDenoiseEnabled; }
    void SetShadowDenoiseEnabled(const bool enabled) { m_shadowDenoiseEnabled = enabled; }

    // Phase D9 — one-bounce RT diffuse GI (devdoc/dxr-diffuse-gi.md). Adds ray-traced diffuse
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
    float m_ptAmbientStrength = 1.0f;
    int m_ptAmbientAoRayCount = 0;
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
