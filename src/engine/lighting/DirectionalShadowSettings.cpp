#include "engine/lighting/DirectionalShadowSettings.h"

#include <nlohmann/json.hpp>

const char* DirectionalShadowSettings::FilterModeToString(const DirectionalShadowFilterMode mode)
{
    return mode == DirectionalShadowFilterMode::PCSS ? "PCSS" : "PCF";
}

DirectionalShadowFilterMode DirectionalShadowSettings::FilterModeFromString(const std::string& value)
{
    if (value == "PCSS")
    {
        return DirectionalShadowFilterMode::PCSS;
    }

    return DirectionalShadowFilterMode::PCF;
}

nlohmann::json DirectionalShadowSettings::ToJson() const
{
    return nlohmann::json{
        {"filterMode", FilterModeToString(m_filterMode)},
        {"shadowMapResolution", m_shadowMapResolution},
        {"cascadeCount", m_cascadeCount},
        {"cascadeSplitLambda", m_cascadeSplitLambda},
        {"cascadeBlendRatio", m_cascadeBlendRatio},
        {"tightNearPlaneXyFit", m_tightNearPlaneXyFit},
        {"xyMarginFraction", m_xyMarginFraction},
        {"zMarginFraction", m_zMarginFraction},
        {"usePoissonPcf", m_usePoissonPcf},
        {"pcfKernelRadius", m_pcfKernelRadius},
        {"pcfSampleCount", m_pcfSampleCount},
        {"minPenumbraTexels", m_minPenumbraTexels},
        {"sunAngularDiameterDegrees", m_sunAngularDiameterDegrees},
        {"pcssLightAngularSize", m_pcssLightAngularSize},
        {"pcssBlockerRadius", m_pcssBlockerRadius},
        {"pcssMinPenumbraTexels", m_pcssMinPenumbraTexels},
        {"pcssMaxPenumbraTexels", m_pcssMaxPenumbraTexels},
        {"worldBiasScale", m_worldBiasScale},
        {"depthBiasScale", m_depthBiasScale},
        {"casterDepthBiasScale", m_casterDepthBiasScale},
        {"shadowBlurEnabled", m_shadowBlurEnabled},
        {"shadowBlurRadius", m_shadowBlurRadius},
        {"shadowBlurDepthThreshold", m_shadowBlurDepthThreshold},
        {"shadowBlurShadowThreshold", m_shadowBlurShadowThreshold},
    };
}

void DirectionalShadowSettings::ApplyFromJson(const nlohmann::json& value)
{
    if (value.contains("filterMode"))
    {
        SetFilterMode(FilterModeFromString(value.at("filterMode").get<std::string>()));
    }
    if (value.contains("shadowMapResolution"))
    {
        SetShadowMapResolution(value.at("shadowMapResolution").get<int>());
    }
    if (value.contains("cascadeCount"))
    {
        SetCascadeCount(value.at("cascadeCount").get<int>());
    }
    if (value.contains("cascadeSplitLambda"))
    {
        SetCascadeSplitLambda(value.at("cascadeSplitLambda").get<float>());
    }
    if (value.contains("cascadeBlendRatio"))
    {
        SetCascadeBlendRatio(value.at("cascadeBlendRatio").get<float>());
    }
    if (value.contains("tightNearPlaneXyFit"))
    {
        SetTightNearPlaneXyFit(value.at("tightNearPlaneXyFit").get<bool>());
    }
    if (value.contains("xyMarginFraction"))
    {
        SetXyMarginFraction(value.at("xyMarginFraction").get<float>());
    }
    if (value.contains("zMarginFraction"))
    {
        SetZMarginFraction(value.at("zMarginFraction").get<float>());
    }
    if (value.contains("usePoissonPcf"))
    {
        SetUsePoissonPcf(value.at("usePoissonPcf").get<bool>());
    }
    if (value.contains("pcfKernelRadius"))
    {
        SetPcfKernelRadius(value.at("pcfKernelRadius").get<int>());
    }
    if (value.contains("pcfSampleCount"))
    {
        SetPcfSampleCount(value.at("pcfSampleCount").get<int>());
    }
    if (value.contains("minPenumbraTexels"))
    {
        SetMinPenumbraTexels(value.at("minPenumbraTexels").get<float>());
    }
    if (value.contains("sunAngularDiameterDegrees"))
    {
        SetSunAngularDiameterDegrees(value.at("sunAngularDiameterDegrees").get<float>());
    }
    if (value.contains("pcssLightAngularSize"))
    {
        SetPcssLightAngularSize(value.at("pcssLightAngularSize").get<float>());
    }
    if (value.contains("pcssBlockerRadius"))
    {
        SetPcssBlockerRadius(value.at("pcssBlockerRadius").get<int>());
    }
    if (value.contains("pcssMinPenumbraTexels"))
    {
        SetPcssMinPenumbraTexels(value.at("pcssMinPenumbraTexels").get<float>());
    }
    if (value.contains("pcssMaxPenumbraTexels"))
    {
        SetPcssMaxPenumbraTexels(value.at("pcssMaxPenumbraTexels").get<float>());
    }
    if (value.contains("worldBiasScale"))
    {
        SetWorldBiasScale(value.at("worldBiasScale").get<float>());
    }
    if (value.contains("depthBiasScale"))
    {
        SetDepthBiasScale(value.at("depthBiasScale").get<float>());
    }
    if (value.contains("casterDepthBiasScale"))
    {
        SetCasterDepthBiasScale(value.at("casterDepthBiasScale").get<float>());
    }
    if (value.contains("shadowBlurEnabled"))
    {
        SetShadowBlurEnabled(value.at("shadowBlurEnabled").get<bool>());
    }
    if (value.contains("shadowBlurRadius"))
    {
        SetShadowBlurRadius(value.at("shadowBlurRadius").get<float>());
    }
    if (value.contains("shadowBlurDepthThreshold"))
    {
        SetShadowBlurDepthThreshold(value.at("shadowBlurDepthThreshold").get<float>());
    }
    if (value.contains("shadowBlurShadowThreshold"))
    {
        SetShadowBlurShadowThreshold(value.at("shadowBlurShadowThreshold").get<float>());
    }
}
