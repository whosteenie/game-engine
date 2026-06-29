#pragma once

#include <string>

enum class DxrReflectionsQuality
{
    Low,
    Medium,
    High,
};

class DxrSettings
{
public:
    static const char* ReflectionsQualityToString(DxrReflectionsQuality quality);
    static DxrReflectionsQuality ReflectionsQualityFromString(const std::string& value);

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

    float GetTemporalBlend() const { return m_temporalBlend; }
    void SetTemporalBlend(const float blend);

    void CopySettingsFrom(const DxrSettings& source);
    void ClampToHardwareCapabilities(bool raytracingSupported);

private:
    bool m_enabled = false;
    bool m_reflectionsEnabled = false;
    DxrReflectionsQuality m_reflectionsQuality = DxrReflectionsQuality::Medium;
    int m_reflectionsSamplesPerPixel = 1;
    float m_maxTraceDistance = 100.0f;
    bool m_denoiseEnabled = true;
    float m_temporalBlend = 0.95f;
};
