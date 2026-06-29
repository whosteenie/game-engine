#pragma once

#include <cstdint>

// Human-readable labels for D3D12_RAYTRACING_TIER values (0, 10, 11).
inline const char* GetRaytracingTierLabel(const int raytracingTier)
{
    switch (raytracingTier)
    {
    case 10:
        return "Tier 1.0";
    case 11:
        return "Tier 1.1";
    case 0:
    default:
        return "Not supported";
    }
}
