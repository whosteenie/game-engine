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

    void CopySettingsFrom(const DxrSettings& source);
    void ClampToHardwareCapabilities(bool raytracingSupported);

private:
    bool m_enabled = false;
    bool m_reflectionsEnabled = false;
    DxrReflectionsQuality m_reflectionsQuality = DxrReflectionsQuality::Medium;
    int m_reflectionsSamplesPerPixel = 1;
    float m_maxTraceDistance = 100.0f;
    bool m_denoiseEnabled = true;
    bool m_debugTraceEnabled = false;
    float m_temporalBlend = 0.95f;
    int m_reflectionAtrousIterations = 5;
    bool m_reflectionAntiFirefly = true;
};
