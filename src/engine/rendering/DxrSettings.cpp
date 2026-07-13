#include "engine/rendering/DxrSettings.h"

#include "engine/platform/EngineLog.h"

#include <nlohmann/json.hpp>

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

const char* DxrSettings::PtConvergenceModeToString(const PtConvergenceMode mode)
{
    switch (mode)
    {
    case PtConvergenceMode::Reference:
        return "reference";
    case PtConvergenceMode::RealTime:
    default:
        return "realTime";
    }
}

PtConvergenceMode DxrSettings::PtConvergenceModeFromString(const std::string& value)
{
    if (value == "reference")
    {
        return PtConvergenceMode::Reference;
    }

    return PtConvergenceMode::RealTime;
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

void DxrSettings::SetPtMaxBounces(const int bounces)
{
    m_ptMaxBounces = std::clamp(bounces, 1, 16);
}

void DxrSettings::SetPtAmbientStrength(const float strength)
{
    m_ptAmbientStrength = std::clamp(strength, 0.0f, 2.0f);
}

void DxrSettings::SetPtAmbientAoRayCount(const int rays)
{
    m_ptAmbientAoRayCount = std::clamp(rays, 0, 8);
}

void DxrSettings::CopySettingsFrom(const DxrSettings& source)
{
    m_enabled = source.m_enabled;
    m_renderingMode = source.m_renderingMode;
    m_ptConvergenceMode = source.m_ptConvergenceMode;
    m_ptMaxBounces = source.m_ptMaxBounces;
    m_ptRussianRoulette = source.m_ptRussianRoulette;
    m_ptFireflyClamp = source.m_ptFireflyClamp;
    m_ptAmbientStrength = source.m_ptAmbientStrength;
    m_ptAmbientAoRayCount = source.m_ptAmbientAoRayCount;
    m_restirDiCandidateCount = source.m_restirDiCandidateCount;
    m_restirDiTemporalEnabled = source.m_restirDiTemporalEnabled;
    m_restirGiInitialEnabled = source.m_restirGiInitialEnabled;
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
    SetPtMaxBounces(m_ptMaxBounces);
    SetPtAmbientStrength(m_ptAmbientStrength);
    SetPtAmbientAoRayCount(m_ptAmbientAoRayCount);
}

nlohmann::json DxrSettings::ToJson() const
{
    return nlohmann::json{
        {"enabled", m_enabled},
        {"renderingMode", RenderingModeToString(m_renderingMode)},
        {"ptConvergenceMode", PtConvergenceModeToString(m_ptConvergenceMode)},
        {"ptMaxBounces", m_ptMaxBounces},
        {"ptRussianRoulette", m_ptRussianRoulette},
        {"ptFireflyClamp", m_ptFireflyClamp},
        {"ptAmbientStrength", m_ptAmbientStrength},
        {"ptAmbientAoRayCount", m_ptAmbientAoRayCount},
        {"restirDiCandidateCount", m_restirDiCandidateCount},
        {"restirDiTemporalEnabled", m_restirDiTemporalEnabled},
        {"restirGiInitialEnabled", m_restirGiInitialEnabled},
        {"reflectionsEnabled", m_reflectionsEnabled},
        {"reflectionsQuality", ReflectionsQualityToString(m_reflectionsQuality)},
        {"reflectionsSamplesPerPixel", m_reflectionsSamplesPerPixel},
        {"maxTraceDistance", m_maxTraceDistance},
        {"denoiseEnabled", m_denoiseEnabled},
        {"debugTraceEnabled", m_debugTraceEnabled},
        {"temporalBlend", m_temporalBlend},
        {"reflectionAtrousIterations", m_reflectionAtrousIterations},
        {"reflectionAntiFirefly", m_reflectionAntiFirefly},
        {"reflectionAoRays", m_reflectionAoRays},
        {"reflectionRoughnessCutoff", m_reflectionRoughnessCutoff},
        {"shadowsEnabled", m_shadowsEnabled},
        {"sunAngularRadiusDegrees", m_sunAngularRadiusDegrees},
        {"shadowDenoiseEnabled", m_shadowDenoiseEnabled},
        {"giEnabled", m_giEnabled},
        {"giStrength", m_giStrength},
        {"giDenoiseEnabled", m_giDenoiseEnabled},
    };
}

void DxrSettings::ApplyFromJson(const nlohmann::json& value)
{
    if (value.contains("enabled"))
    {
        SetEnabled(value.at("enabled").get<bool>());
    }
    if (value.contains("renderingMode"))
    {
        SetRenderingMode(RenderingModeFromString(value.at("renderingMode").get<std::string>()));
    }
    if (value.contains("ptConvergenceMode"))
    {
        SetPtConvergenceMode(
            PtConvergenceModeFromString(value.at("ptConvergenceMode").get<std::string>()));
    }
    if (value.contains("ptMaxBounces"))
    {
        SetPtMaxBounces(value.at("ptMaxBounces").get<int>());
    }
    if (value.contains("ptRussianRoulette"))
    {
        SetPtRussianRouletteEnabled(value.at("ptRussianRoulette").get<bool>());
    }
    if (value.contains("ptFireflyClamp"))
    {
        SetPtFireflyClampEnabled(value.at("ptFireflyClamp").get<bool>());
    }
    if (value.contains("ptAmbientStrength"))
    {
        SetPtAmbientStrength(value.at("ptAmbientStrength").get<float>());
    }
    if (value.contains("ptAmbientAoRayCount"))
    {
        SetPtAmbientAoRayCount(value.at("ptAmbientAoRayCount").get<int>());
    }
    if (value.contains("restirDiCandidateCount"))
    {
        SetRestirDiCandidateCount(value.at("restirDiCandidateCount").get<int>());
    }
    if (value.contains("restirDiTemporalEnabled"))
    {
        SetRestirDiTemporalEnabled(value.at("restirDiTemporalEnabled").get<bool>());
    }
    if (value.contains("restirGiInitialEnabled"))
    {
        SetRestirGiInitialEnabled(value.at("restirGiInitialEnabled").get<bool>());
    }
    if (value.contains("reflectionsEnabled"))
    {
        SetReflectionsEnabled(value.at("reflectionsEnabled").get<bool>());
    }
    if (value.contains("reflectionsQuality"))
    {
        SetReflectionsQuality(
            ReflectionsQualityFromString(value.at("reflectionsQuality").get<std::string>()));
    }
    if (value.contains("reflectionsSamplesPerPixel"))
    {
        SetReflectionsSamplesPerPixel(value.at("reflectionsSamplesPerPixel").get<int>());
    }
    if (value.contains("maxTraceDistance"))
    {
        SetMaxTraceDistance(value.at("maxTraceDistance").get<float>());
    }
    if (value.contains("denoiseEnabled"))
    {
        SetDenoiseEnabled(value.at("denoiseEnabled").get<bool>());
    }
    if (value.contains("debugTraceEnabled"))
    {
        SetDebugTraceEnabled(value.at("debugTraceEnabled").get<bool>());
    }
    if (value.contains("temporalBlend"))
    {
        SetTemporalBlend(value.at("temporalBlend").get<float>());
    }
    if (value.contains("reflectionAtrousIterations"))
    {
        SetReflectionAtrousIterations(value.at("reflectionAtrousIterations").get<int>());
    }
    if (value.contains("reflectionAntiFirefly"))
    {
        SetReflectionAntiFireflyEnabled(value.at("reflectionAntiFirefly").get<bool>());
    }
    if (value.contains("reflectionAoRays"))
    {
        SetReflectionAoRays(value.at("reflectionAoRays").get<int>());
    }
    if (value.contains("reflectionRoughnessCutoff"))
    {
        SetReflectionRoughnessCutoff(value.at("reflectionRoughnessCutoff").get<float>());
    }
    if (value.contains("shadowsEnabled"))
    {
        SetShadowsEnabled(value.at("shadowsEnabled").get<bool>());
    }
    if (value.contains("sunAngularRadiusDegrees"))
    {
        SetSunAngularRadiusDegrees(value.at("sunAngularRadiusDegrees").get<float>());
    }
    if (value.contains("shadowDenoiseEnabled"))
    {
        SetShadowDenoiseEnabled(value.at("shadowDenoiseEnabled").get<bool>());
    }
    if (value.contains("giEnabled"))
    {
        SetGiEnabled(value.at("giEnabled").get<bool>());
    }
    if (value.contains("giStrength"))
    {
        SetGiStrength(value.at("giStrength").get<float>());
    }
    if (value.contains("giDenoiseEnabled"))
    {
        SetGiDenoiseEnabled(value.at("giDenoiseEnabled").get<bool>());
    }
}

void DxrSettings::ClampToHardwareWithLogging(const bool raytracingSupported)
{
    const bool hadPathTraced = m_renderingMode == RenderingMode::PathTraced;
    ClampToHardwareCapabilities(raytracingSupported);
    if (hadPathTraced && m_renderingMode != RenderingMode::PathTraced)
    {
        EngineLog::Warn("dxr", "Path traced mode unavailable on this GPU — falling back to Hybrid");
    }
}
