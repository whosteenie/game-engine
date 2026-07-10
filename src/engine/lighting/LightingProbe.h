#pragma once

#include "engine/camera/Camera.h"

#include <glm/glm.hpp>

#include <vector>

struct LightingProbeResult
{
    float viewDepth = 0.0f;
    int cascadeIndex = 0;
    float cascadeBlendFactor = 0.0f;
    float sunDotGeomNormal = 0.0f;
};

LightingProbeResult EvaluateLightingProbe(
    const glm::mat4& viewMatrix,
    const glm::vec3& worldPosition,
    const glm::vec3& geomNormal,
    const glm::vec3& lightDirectionTowardSun,
    const std::vector<float>& cascadeEndSplits,
    float nearPlane,
    float cascadeBlendRatio,
    int cascadeCount);

inline LightingProbeResult EvaluateLightingProbe(
    const Camera& camera,
    const glm::vec3& worldPosition,
    const glm::vec3& geomNormal,
    const glm::vec3& lightDirectionTowardSun,
    const std::vector<float>& cascadeEndSplits,
    const float nearPlane,
    const float cascadeBlendRatio,
    const int cascadeCount)
{
    return EvaluateLightingProbe(
        camera.GetViewMatrix(),
        worldPosition,
        geomNormal,
        lightDirectionTowardSun,
        cascadeEndSplits,
        nearPlane,
        cascadeBlendRatio,
        cascadeCount);
}
