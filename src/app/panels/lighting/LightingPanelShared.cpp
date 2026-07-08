#include "app/panels/lighting/LightingPanelShared.h"

#include "engine/rhi/GfxContext.h"

#include <vector>

float FindGpuPassMilliseconds(const char* passName)
{
    const std::vector<GpuProfiler::Entry>& timings = GfxContext::Get().GetGpuTimings();
    for (const GpuProfiler::Entry& entry : timings)
    {
        if (entry.name == passName)
        {
            return entry.milliseconds;
        }
    }
    return -1.0f;
}

const char* AntiAliasingModeLabel(AntiAliasingMode mode)
{
    switch (mode)
    {
    case AntiAliasingMode::FXAA:
        return "FXAA";
    case AntiAliasingMode::TAA:
        return "TAA";
    case AntiAliasingMode::MSAA:
        return "MSAA (not supported)";
    case AntiAliasingMode::SMAA:
        return "SMAA";
    case AntiAliasingMode::SSAA:
        return "SSAA";
    case AntiAliasingMode::DLAA:
        return "DLAA (DLSS native)";
    case AntiAliasingMode::DLSS:
        return "DLSS Super Resolution";
    case AntiAliasingMode::None:
    default:
        return "None";
    }
}

const char* DlssPresetLabel(DlssPreset preset)
{
    switch (preset)
    {
    case DlssPreset::Quality:
        return "Quality";
    case DlssPreset::Balanced:
        return "Balanced";
    case DlssPreset::Performance:
        return "Performance";
    case DlssPreset::UltraPerformance:
        return "Ultra Performance";
    default:
        return "Quality";
    }
}

const char* AmbientOcclusionModeLabel(AmbientOcclusionMode mode)
{
    switch (mode)
    {
    case AmbientOcclusionMode::SSAO:
        return "SSAO";
    case AmbientOcclusionMode::GTAO:
        return "GTAO";
    case AmbientOcclusionMode::Off:
    default:
        return "Off";
    }
}

int IblCubemapResolutionToComboIndex(const EnvironmentIblCubemapResolution resolution)
{
    switch (resolution)
    {
    case EnvironmentIblCubemapResolution::Size512:
        return 1;
    case EnvironmentIblCubemapResolution::Size1024:
        return 2;
    case EnvironmentIblCubemapResolution::Size2048:
        return 3;
    case EnvironmentIblCubemapResolution::Size4096:
        return 4;
    case EnvironmentIblCubemapResolution::Auto:
    default:
        return 0;
    }
}

EnvironmentIblCubemapResolution IblCubemapResolutionFromComboIndex(const int index)
{
    switch (index)
    {
    case 1:
        return EnvironmentIblCubemapResolution::Size512;
    case 2:
        return EnvironmentIblCubemapResolution::Size1024;
    case 3:
        return EnvironmentIblCubemapResolution::Size2048;
    case 4:
        return EnvironmentIblCubemapResolution::Size4096;
    case 0:
    default:
        return EnvironmentIblCubemapResolution::Auto;
    }
}
