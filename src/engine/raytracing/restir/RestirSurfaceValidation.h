#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

struct RestirSurfaceValidationRecord
{
    float linearDepth = 0.0f;
    std::array<float, 3> worldPosition{};
    std::array<float, 3> geometricNormal{0.0f, 1.0f, 0.0f};
    std::array<float, 3> shadingNormal{0.0f, 1.0f, 0.0f};
    float roughness = 1.0f;
    std::uint32_t materialId = 0;
    std::uint32_t instanceId = 0;
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

    (void)depthThreshold;
    if (current.instanceId != history.instanceId)
    {
        return false;
    }

    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const float geomNormalDot = dot(current.geometricNormal, history.geometricNormal);
    if (geomNormalDot < normalThreshold)
    {
        return false;
    }

    const std::array<float, 3> worldDelta{
        history.worldPosition[0] - current.worldPosition[0],
        history.worldPosition[1] - current.worldPosition[1],
        history.worldPosition[2] - current.worldPosition[2]};
    const float depthScale = std::max(std::max(current.linearDepth, history.linearDepth), 1.0f);
    const float planeTolerance = std::max(0.005f, 0.0025f * depthScale);
    if (std::fabs(dot(worldDelta, current.geometricNormal)) > planeTolerance
        || std::fabs(dot(worldDelta, history.geometricNormal)) > planeTolerance)
    {
        return false;
    }
    const float alongNormal = dot(worldDelta, current.geometricNormal);
    const std::array<float, 3> tangentDelta{
        worldDelta[0] - current.geometricNormal[0] * alongNormal,
        worldDelta[1] - current.geometricNormal[1] * alongNormal,
        worldDelta[2] - current.geometricNormal[2] * alongNormal};
    if (std::sqrt(dot(tangentDelta, tangentDelta)) > std::max(0.02f, 0.01f * depthScale))
    {
        return false;
    }

    const float shadingNormalDot = dot(current.shadingNormal, history.shadingNormal);
    return shadingNormalDot >= normalThreshold
        && std::fabs(current.roughness - history.roughness) <= roughnessThreshold
        && current.materialId == history.materialId
        && current.lobeFlags == history.lobeFlags;
}
