#include "engine/lighting/LightingProbe.h"

#include <algorithm>
#include <cmath>

namespace
{
    int SelectCascadeIndex(const float viewDepth, const std::vector<float>& cascadeEndSplits, const int cascadeCount)
    {
        int cascadeIndex = 0;
        for (int splitIndex = 0; splitIndex < cascadeCount - 1; ++splitIndex)
        {
            if (viewDepth > cascadeEndSplits[static_cast<std::size_t>(splitIndex)])
            {
                cascadeIndex = splitIndex + 1;
            }
        }
        return cascadeIndex;
    }

    float ComputeCascadeBlendFactor(
        const float viewDepth,
        const std::vector<float>& cascadeEndSplits,
        const float nearPlane,
        const float cascadeBlendRatio,
        const int cascadeCount)
    {
        for (int boundaryIndex = 0; boundaryIndex < cascadeCount - 1; ++boundaryIndex)
        {
            const float splitDistance = cascadeEndSplits[static_cast<std::size_t>(boundaryIndex)];
            float previousSplit = nearPlane;
            if (boundaryIndex > 0)
            {
                previousSplit = cascadeEndSplits[static_cast<std::size_t>(boundaryIndex - 1)];
            }

            const float cascadeSpan = std::max(splitDistance - previousSplit, 1e-4f);
            const float blendWidth = cascadeSpan * cascadeBlendRatio;
            const float blendStart = splitDistance - blendWidth;

            if (viewDepth > blendStart && viewDepth <= splitDistance)
            {
                const float t = (viewDepth - blendStart) / std::max(splitDistance - blendStart, 1e-4f);
                const float smoothT = t * t * (3.0f - 2.0f * t);
                return std::clamp(smoothT, 0.0f, 1.0f);
            }
        }

        return 0.0f;
    }
}

LightingProbeResult EvaluateLightingProbe(
    const glm::mat4& viewMatrix,
    const glm::vec3& worldPosition,
    const glm::vec3& geomNormal,
    const glm::vec3& lightDirectionTowardSun,
    const std::vector<float>& cascadeEndSplits,
    const float nearPlane,
    const float cascadeBlendRatio,
    const int cascadeCount)
{
    LightingProbeResult result;

    const glm::vec4 viewPosition = viewMatrix * glm::vec4(worldPosition, 1.0f);
    result.viewDepth = viewPosition.z;

    if (cascadeCount > 0 && cascadeEndSplits.size() >= static_cast<std::size_t>(cascadeCount))
    {
        result.cascadeIndex = SelectCascadeIndex(result.viewDepth, cascadeEndSplits, cascadeCount);
        result.cascadeBlendFactor = ComputeCascadeBlendFactor(
            result.viewDepth,
            cascadeEndSplits,
            nearPlane,
            cascadeBlendRatio,
            cascadeCount);
    }

    const glm::vec3 normalizedLightDirection = glm::normalize(lightDirectionTowardSun);
    const glm::vec3 normalizedGeomNormal = glm::normalize(geomNormal);
    result.sunDotGeomNormal = glm::dot(normalizedGeomNormal, normalizedLightDirection);

    return result;
}
