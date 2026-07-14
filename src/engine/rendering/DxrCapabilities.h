#pragma once

#include <d3d12.h>

#include <cstdio>
#include <cstddef>

// Human-readable label for D3D12_RAYTRACING_TIER (uses SDK enum values).
inline const char* GetRaytracingTierLabel(const int raytracingTier)
{
    if (raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_2))
    {
        return "Tier 1.2";
    }
    if (raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_1))
    {
        return "Tier 1.1";
    }
    if (raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_0))
    {
        return "Tier 1.0";
    }
    if (raytracingTier > 0)
    {
        return "Tier 1.0+ (non-standard code)";
    }
    return "Not supported";
}

inline const char* GetShaderModelLabel(const int shaderModel)
{
    switch (shaderModel)
    {
    case D3D_SHADER_MODEL_6_9: return "6.9";
    case D3D_SHADER_MODEL_6_8: return "6.8";
    case D3D_SHADER_MODEL_6_7: return "6.7";
    case D3D_SHADER_MODEL_6_6: return "6.6";
    case D3D_SHADER_MODEL_6_5: return "6.5";
    case D3D_SHADER_MODEL_6_4: return "6.4";
    case D3D_SHADER_MODEL_6_3: return "6.3";
    case D3D_SHADER_MODEL_6_2: return "6.2";
    case D3D_SHADER_MODEL_6_1: return "6.1";
    case D3D_SHADER_MODEL_6_0: return "6.0";
    default: return shaderModel > 0 ? "Unknown" : "Not supported";
    }
}

// Capability policy shared by device reporting, DXR library selection, and tests. PF6 uses
// inline visibility when supported; SER remains an availability check until PF7.
struct DxrFeatureCapabilities
{
    int raytracingTier = 0;
    int highestShaderModel = 0;

    bool SupportsInlineRaytracing() const
    {
        return raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_1)
            && highestShaderModel >= static_cast<int>(D3D_SHADER_MODEL_6_5);
    }

    bool SupportsShaderExecutionReordering() const
    {
        return raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_2)
            && highestShaderModel >= static_cast<int>(D3D_SHADER_MODEL_6_6);
    }

    bool SupportsModernDxrLibrary() const
    {
        return raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_0)
            && highestShaderModel >= static_cast<int>(D3D_SHADER_MODEL_6_6);
    }

    const char* GetPreferredLibraryProfile() const
    {
        if (SupportsModernDxrLibrary())
        {
            return "lib_6_6";
        }
        // Inline RayQuery was introduced with SM 6.5. Keep that capable tier on a matching
        // library target even if it cannot expose the SM 6.6+ PF5/PF7 feature set.
        return SupportsInlineRaytracing() ? "lib_6_5" : "lib_6_3";
    }
};

// Label plus raw tier integer for the editor panel.
inline void FormatRaytracingTierText(const int raytracingTier, char* buffer, const std::size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0)
    {
        return;
    }

    if (raytracingTier <= 0)
    {
        std::snprintf(buffer, bufferSize, "%s", GetRaytracingTierLabel(raytracingTier));
        return;
    }

    std::snprintf(
        buffer,
        bufferSize,
        "%s (%d)",
        GetRaytracingTierLabel(raytracingTier),
        raytracingTier);
}
