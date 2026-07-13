#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

struct RestirSurfaceValidationRecord
{
    float linearDepth = 0.0f;
    std::array<float, 3> shadingNormal{0.0f, 1.0f, 0.0f};
    float roughness = 1.0f;
    std::uint32_t materialId = 0;
    std::uint32_t lobeFlags = 0; // bit 0 transmission, bit 1 delta
    bool valid = false;
};

inline bool AreRestirSurfacesCompatible(
    const RestirSurfaceValidationRecord& current,
    const RestirSurfaceValidationRecord& history,
    const float depthThreshold = 0.02f,
    const float normalThreshold = 0.9f,
    const float roughnessThreshold = 0.1f)
{
    if (!current.valid || !history.valid || current.linearDepth <= 0.0f || history.linearDepth <= 0.0f)
    {
        return false;
    }

    const float depthScale = std::max(std::max(current.linearDepth, history.linearDepth), 1.0e-3f);
    if (std::fabs(current.linearDepth - history.linearDepth) > depthThreshold * depthScale)
    {
        return false;
    }

    const float shadingNormalDot =
        current.shadingNormal[0] * history.shadingNormal[0]
        + current.shadingNormal[1] * history.shadingNormal[1]
        + current.shadingNormal[2] * history.shadingNormal[2];
    return shadingNormalDot >= normalThreshold
        && std::fabs(current.roughness - history.roughness) <= roughnessThreshold
        && current.materialId == history.materialId
        && current.lobeFlags == history.lobeFlags;
}
