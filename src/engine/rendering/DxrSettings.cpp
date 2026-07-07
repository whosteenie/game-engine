#include "engine/rendering/DxrSettings.h"

#include <algorithm>

const char* DxrSettings::ReflectionsQualityToString(const DxrReflectionsQuality quality)
{
    switch (quality)
    {
    case DxrReflectionsQuality::Low:
        return "low";
    case DxrReflectionsQuality::High:
        return "high";
    case DxrReflectionsQuality::Medium:
    default:
        return "medium";
    }
}

DxrReflectionsQuality DxrSettings::ReflectionsQualityFromString(const std::string& value)
{
    if (value == "low")
    {
        return DxrReflectionsQuality::Low;
    }
    if (value == "high")
    {
        return DxrReflectionsQuality::High;
    }

    return DxrReflectionsQuality::Medium;
}

const char* DxrSettings::RenderingModeToString(const RenderingMode mode)
{
    switch (mode)
    {
    case RenderingMode::PathTraced:
        return "pathTraced";
    case RenderingMode::Hybrid:
    default:
        return "hybrid";
    }
}

RenderingMode DxrSettings::RenderingModeFromString(const std::string& value)
{
    if (value == "pathTraced")
    {
        return RenderingMode::PathTraced;
    }

    return RenderingMode::Hybrid;
}

void DxrSettings::SetReflectionsSamplesPerPixel(const int samples)
{
    m_reflectionsSamplesPerPixel = std::clamp(samples, 1, 16);
}

void DxrSettings::SetMaxTraceDistance(const float distance)
{
    m_maxTraceDistance = std::clamp(distance, 1.0f, 500.0f);
}

void DxrSettings::SetTemporalBlend(const float blend)
{
    m_temporalBlend = std::clamp(blend, 0.0f, 0.99f);
}

void DxrSettings::SetReflectionAtrousIterations(const int iterations)
{
    m_reflectionAtrousIterations = std::clamp(iterations, 2, 8);
}

void DxrSettings::SetReflectionAoRays(const int rays)
{
    m_reflectionAoRays = std::clamp(rays, 0, 16);
}

void DxrSettings::SetReflectionRoughnessCutoff(const float cutoff)
{
    m_reflectionRoughnessCutoff = std::clamp(cutoff, 0.0f, 1.0f);
}

void DxrSettings::SetSunAngularRadiusDegrees(const float degrees)
{
    m_sunAngularRadiusDegrees = std::clamp(degrees, 0.05f, 2.0f);
}

void DxrSettings::SetGiStrength(const float strength)
{
    m_giStrength = std::clamp(strength, 0.0f, 2.0f);
}

void DxrSettings::CopySettingsFrom(const DxrSettings& source)
{
    m_enabled = source.m_enabled;
    m_renderingMode = source.m_renderingMode;
    m_reflectionsEnabled = source.m_reflectionsEnabled;
    m_reflectionsQuality = source.m_reflectionsQuality;
    m_reflectionsSamplesPerPixel = source.m_reflectionsSamplesPerPixel;
    m_maxTraceDistance = source.m_maxTraceDistance;
    m_denoiseEnabled = source.m_denoiseEnabled;
    m_debugTraceEnabled = source.m_debugTraceEnabled;
    m_temporalBlend = source.m_temporalBlend;
    m_reflectionAtrousIterations = source.m_reflectionAtrousIterations;
    m_reflectionAntiFirefly = source.m_reflectionAntiFirefly;
    m_reflectionAoRays = source.m_reflectionAoRays;
    m_reflectionRoughnessCutoff = source.m_reflectionRoughnessCutoff;
    m_shadowsEnabled = source.m_shadowsEnabled;
    m_sunAngularRadiusDegrees = source.m_sunAngularRadiusDegrees;
    m_shadowDenoiseEnabled = source.m_shadowDenoiseEnabled;
    m_giEnabled = source.m_giEnabled;
    m_giStrength = source.m_giStrength;
    m_giDenoiseEnabled = source.m_giDenoiseEnabled;
}

void DxrSettings::ClampToHardwareCapabilities(const bool raytracingSupported)
{
    if (!raytracingSupported)
    {
        m_enabled = false;
        m_renderingMode = RenderingMode::Hybrid;
        m_reflectionsEnabled = false;
        m_shadowsEnabled = false;
        m_giEnabled = false;
    }

    SetReflectionsSamplesPerPixel(m_reflectionsSamplesPerPixel);
    SetMaxTraceDistance(m_maxTraceDistance);
    SetTemporalBlend(m_temporalBlend);
    SetReflectionAtrousIterations(m_reflectionAtrousIterations);
    SetReflectionAoRays(m_reflectionAoRays);
    SetReflectionRoughnessCutoff(m_reflectionRoughnessCutoff);
    SetSunAngularRadiusDegrees(m_sunAngularRadiusDegrees);
    SetGiStrength(m_giStrength);
}
