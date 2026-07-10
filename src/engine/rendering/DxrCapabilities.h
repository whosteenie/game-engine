#pragma once

#include <d3d12.h>

#include <cstdio>
#include <cstddef>

// Human-readable label for D3D12_RAYTRACING_TIER (uses SDK enum values).
inline const char* GetRaytracingTierLabel(const int raytracingTier)
{
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
