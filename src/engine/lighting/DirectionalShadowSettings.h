#pragma once

#include <algorithm>

enum class DirectionalShadowFilterMode
{
    PCF = 0,
    PCSS = 1,
};

class DirectionalShadowSettings
{
public:
    static constexpr int MaxCascades = 4;

    DirectionalShadowFilterMode GetFilterMode() const { return m_filterMode; }
    void SetFilterMode(DirectionalShadowFilterMode mode) { m_filterMode = mode; }

    int GetShadowMapResolution() const { return m_shadowMapResolution; }
    void SetShadowMapResolution(int resolution)
    {
        m_shadowMapResolution = std::clamp(resolution, 512, 8192);
    }

    // CSM disabled for now — single shadow map only (MaxCascades kept for texture array layout).
    int GetCascadeCount() const { return 1; }
    void SetCascadeCount(int /*count*/) { m_cascadeCount = 1; }

    float GetCascadeSplitLambda() const { return m_cascadeSplitLambda; }
    void SetCascadeSplitLambda(float lambda) { m_cascadeSplitLambda = std::clamp(lambda, 0.0f, 1.0f); }

    float GetCascadeBlendRatio() const { return m_cascadeBlendRatio; }
    void SetCascadeBlendRatio(float ratio) { m_cascadeBlendRatio = std::clamp(ratio, 0.0f, 0.5f); }

    bool GetTightNearPlaneXyFit() const { return m_tightNearPlaneXyFit; }
    void SetTightNearPlaneXyFit(bool enabled) { m_tightNearPlaneXyFit = enabled; }
    // When enabled, ortho XY is fit to the view frustum only; caster bounds still expand Z depth.

    float GetXyMarginFraction() const { return m_xyMarginFraction; }
    void SetXyMarginFraction(float fraction) { m_xyMarginFraction = std::clamp(fraction, 0.005f, 0.2f); }

    float GetZMarginFraction() const { return m_zMarginFraction; }
    void SetZMarginFraction(float fraction) { m_zMarginFraction = std::clamp(fraction, 0.02f, 0.5f); }

    bool GetUsePoissonPcf() const { return m_usePoissonPcf; }
    void SetUsePoissonPcf(bool enabled) { m_usePoissonPcf = enabled; }

    int GetPcfKernelRadius() const { return m_pcfKernelRadius; }
    void SetPcfKernelRadius(int radius) { m_pcfKernelRadius = std::clamp(radius, 1, 8); }

    int GetPcfSampleCount() const { return m_pcfSampleCount; }
    void SetPcfSampleCount(int count) { m_pcfSampleCount = std::clamp(count, 8, 32); }

    float GetMinPenumbraTexels() const { return m_minPenumbraTexels; }
    void SetMinPenumbraTexels(float texels) { m_minPenumbraTexels = std::clamp(texels, 0.0f, 16.0f); }

    float GetSunAngularDiameterDegrees() const { return m_sunAngularDiameterDegrees; }
    void SetSunAngularDiameterDegrees(float degrees)
    {
        m_sunAngularDiameterDegrees = std::clamp(degrees, 0.0f, 5.0f);
    }

    float GetPcssLightAngularSize() const { return m_pcssLightAngularSize; }
    void SetPcssLightAngularSize(float size) { m_pcssLightAngularSize = std::clamp(size, 0.25f, 24.0f); }

    int GetPcssBlockerRadius() const { return m_pcssBlockerRadius; }
    void SetPcssBlockerRadius(int radius) { m_pcssBlockerRadius = std::clamp(radius, 1, 6); }

    float GetPcssMinPenumbraTexels() const { return m_pcssMinPenumbraTexels; }
    void SetPcssMinPenumbraTexels(float texels) { m_pcssMinPenumbraTexels = std::clamp(texels, 0.5f, 16.0f); }

    float GetPcssMaxPenumbraTexels() const { return m_pcssMaxPenumbraTexels; }
    void SetPcssMaxPenumbraTexels(float texels)
    {
        m_pcssMaxPenumbraTexels = std::clamp(texels, m_pcssMinPenumbraTexels, 64.0f);
    }

    float GetWorldBiasScale() const { return m_worldBiasScale; }
    void SetWorldBiasScale(float scale) { m_worldBiasScale = std::clamp(scale, 0.0f, 4.0f); }

    float GetDepthBiasScale() const { return m_depthBiasScale; }
    void SetDepthBiasScale(float scale) { m_depthBiasScale = std::clamp(scale, 0.0f, 4.0f); }

    bool GetShadowBlurEnabled() const { return m_shadowBlurEnabled; }
    void SetShadowBlurEnabled(bool enabled) { m_shadowBlurEnabled = enabled; }

    float GetShadowBlurRadius() const { return m_shadowBlurRadius; }
    void SetShadowBlurRadius(float radius) { m_shadowBlurRadius = std::clamp(radius, 0.0f, 8.0f); }

private:
    DirectionalShadowFilterMode m_filterMode = DirectionalShadowFilterMode::PCF;
    int m_shadowMapResolution = 4096;
    int m_cascadeCount = 1;
    float m_cascadeSplitLambda = 0.82f;
    float m_cascadeBlendRatio = 0.08f;
    bool m_tightNearPlaneXyFit = true;
    float m_xyMarginFraction = 0.05f;
    float m_zMarginFraction = 0.10f;
    bool m_usePoissonPcf = false;
    int m_pcfKernelRadius = 2;
    int m_pcfSampleCount = 32;
    float m_minPenumbraTexels = 2.5f;
    float m_sunAngularDiameterDegrees = 0.5f;
    float m_pcssLightAngularSize = 3.0f;
    int m_pcssBlockerRadius = 2;
    float m_pcssMinPenumbraTexels = 2.0f;
    float m_pcssMaxPenumbraTexels = 32.0f;
    float m_worldBiasScale = 1.0f;
    float m_depthBiasScale = 1.0f;
    bool m_shadowBlurEnabled = false;
    float m_shadowBlurRadius = 2.5f;
};
