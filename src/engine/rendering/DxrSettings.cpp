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

void DxrSettings::SetSunAngularRadiusDegrees(const float degrees)
{
    m_sunAngularRadiusDegrees = std::clamp(degrees, 0.05f, 2.0f);
}

void DxrSettings::CopySettingsFrom(const DxrSettings& source)
{
    m_enabled = source.m_enabled;
    m_reflectionsEnabled = source.m_reflectionsEnabled;
    m_reflectionsQuality = source.m_reflectionsQuality;
    m_reflectionsSamplesPerPixel = source.m_reflectionsSamplesPerPixel;
    m_maxTraceDistance = source.m_maxTraceDistance;
    m_denoiseEnabled = source.m_denoiseEnabled;
    m_debugTraceEnabled = source.m_debugTraceEnabled;
    m_temporalBlend = source.m_temporalBlend;
    m_reflectionAtrousIterations = source.m_reflectionAtrousIterations;
    m_reflectionAntiFirefly = source.m_reflectionAntiFirefly;
    m_shadowsEnabled = source.m_shadowsEnabled;
    m_sunAngularRadiusDegrees = source.m_sunAngularRadiusDegrees;
    m_shadowDenoiseEnabled = source.m_shadowDenoiseEnabled;
}

void DxrSettings::ClampToHardwareCapabilities(const bool raytracingSupported)
{
    if (!raytracingSupported)
    {
        m_enabled = false;
        m_reflectionsEnabled = false;
        m_shadowsEnabled = false;
    }

    SetReflectionsSamplesPerPixel(m_reflectionsSamplesPerPixel);
    SetMaxTraceDistance(m_maxTraceDistance);
    SetTemporalBlend(m_temporalBlend);
    SetReflectionAtrousIterations(m_reflectionAtrousIterations);
    SetSunAngularRadiusDegrees(m_sunAngularRadiusDegrees);
}
