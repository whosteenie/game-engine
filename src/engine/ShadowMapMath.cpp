#include "engine/ShadowMapMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    std::array<glm::vec3, 8> BuildAxisAlignedBoxCorners(const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        return {
            glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
        };
    }

    ShadowLightSpaceSetup BuildShadowLightSpaceFromWorldPoints(
        const glm::vec3& lightDirectionTowardSource,
        const std::vector<glm::vec3>& xyWorldPoints,
        const std::vector<glm::vec3>& zWorldPoints,
        const int shadowMapResolution,
        const float xyMarginFraction,
        const float zMarginFraction,
        const glm::vec3* lightViewAnchor,
        float* stableOrthoHalfExtentInOut,
        const bool allowOrthoShrink)
    {
        ShadowLightSpaceSetup setup;
        if (zWorldPoints.empty())
        {
            return setup;
        }

        const std::vector<glm::vec3>& centerPoints =
            xyWorldPoints.empty() ? zWorldPoints : xyWorldPoints;

        setup.sceneCenter = glm::vec3(0.0f);
        for (const glm::vec3& point : centerPoints)
        {
            setup.sceneCenter += point;
        }
        setup.sceneCenter /= static_cast<float>(centerPoints.size());

        const glm::vec3 normalizedLightDirection = glm::normalize(lightDirectionTowardSource);
        const glm::vec3 viewTarget =
            lightViewAnchor != nullptr ? *lightViewAnchor : setup.sceneCenter;
        const glm::vec3 lightEye = viewTarget + normalizedLightDirection * 50.0f;
        setup.lightView = glm::lookAt(lightEye, viewTarget, glm::vec3(0.0f, 1.0f, 0.0f));

        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();

        const auto expandXy = [&](const glm::vec3& point) {
            const glm::vec4 lightSpaceCorner = setup.lightView * glm::vec4(point, 1.0f);
            minX = std::min(minX, lightSpaceCorner.x);
            maxX = std::max(maxX, lightSpaceCorner.x);
            minY = std::min(minY, lightSpaceCorner.y);
            maxY = std::max(maxY, lightSpaceCorner.y);
        };

        const auto expandZ = [&](const glm::vec3& point) {
            const glm::vec4 lightSpaceCorner = setup.lightView * glm::vec4(point, 1.0f);
            minZ = std::min(minZ, lightSpaceCorner.z);
            maxZ = std::max(maxZ, lightSpaceCorner.z);
        };

        if (xyWorldPoints.empty())
        {
            for (const glm::vec3& point : zWorldPoints)
            {
                expandXy(point);
            }
        }
        else
        {
            for (const glm::vec3& point : xyWorldPoints)
            {
                expandXy(point);
            }
        }

        for (const glm::vec3& point : zWorldPoints)
        {
            expandZ(point);
        }

        const float spanX = std::max(maxX - minX, 1e-3f);
        const float spanY = std::max(maxY - minY, 1e-3f);
        const float spanZ = std::max(maxZ - minZ, 1e-3f);
        const float marginX = std::max(0.25f, spanX * xyMarginFraction);
        const float marginY = std::max(0.25f, spanY * xyMarginFraction);
        const float marginZ = std::max(1.0f, spanZ * zMarginFraction);

        float centerX = (minX + maxX) * 0.5f;
        float centerY = (minY + maxY) * 0.5f;
        float halfExtent = std::max(spanX * 0.5f + marginX, spanY * 0.5f + marginY);

        if (lightViewAnchor != nullptr)
        {
            const glm::vec3 anchorLight =
                glm::vec3(setup.lightView * glm::vec4(*lightViewAnchor, 1.0f));
            centerX = anchorLight.x;
            centerY = anchorLight.y;
            halfExtent = 0.0f;

            const std::vector<glm::vec3>& extentPoints =
                xyWorldPoints.empty() ? zWorldPoints : xyWorldPoints;
            for (const glm::vec3& point : extentPoints)
            {
                const glm::vec3 lightSpacePoint =
                    glm::vec3(setup.lightView * glm::vec4(point, 1.0f));
                const float extentX = std::abs(lightSpacePoint.x - centerX);
                const float extentY = std::abs(lightSpacePoint.y - centerY);
                halfExtent = std::max(halfExtent, std::max(extentX, extentY));
            }

            const float extentMargin = std::max(0.25f, halfExtent * xyMarginFraction);
            halfExtent += extentMargin;
        }

        float texelWorldSize = (halfExtent * 2.0f) / static_cast<float>(shadowMapResolution);
        halfExtent = std::ceil(halfExtent / texelWorldSize) * texelWorldSize;
        texelWorldSize = (halfExtent * 2.0f) / static_cast<float>(shadowMapResolution);

        if (stableOrthoHalfExtentInOut != nullptr)
        {
            if (allowOrthoShrink || *stableOrthoHalfExtentInOut <= 0.0f || halfExtent > *stableOrthoHalfExtentInOut)
            {
                *stableOrthoHalfExtentInOut = halfExtent;
            }
            else if (halfExtent < *stableOrthoHalfExtentInOut * 0.92f)
            {
                *stableOrthoHalfExtentInOut = halfExtent;
            }

            halfExtent = *stableOrthoHalfExtentInOut;
            texelWorldSize = (halfExtent * 2.0f) / static_cast<float>(shadowMapResolution);
        }

        const float snappedCenterX =
            std::floor(centerX / texelWorldSize) * texelWorldSize + texelWorldSize * 0.5f;
        const float snappedCenterY =
            std::floor(centerY / texelWorldSize) * texelWorldSize + texelWorldSize * 0.5f;

        setup.orthoWidth = halfExtent * 2.0f;
        setup.orthoHeight = halfExtent * 2.0f;
        setup.texelWorldSizeX = texelWorldSize;
        setup.texelWorldSizeY = texelWorldSize;

        setup.lightProjection = glm::ortho(
            snappedCenterX - halfExtent,
            snappedCenterX + halfExtent,
            snappedCenterY - halfExtent,
            snappedCenterY + halfExtent,
            -maxZ - marginZ,
            -minZ + marginZ);

        setup.lightSpaceMatrix = setup.lightProjection * setup.lightView;
        setup.snapOffsetNdc = glm::vec2(0.0f);

        return setup;
    }
}

ShadowLightSpaceSetup BuildShadowLightSpace(
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const int shadowMapResolution,
    const float margin)
{
    const std::array<glm::vec3, 8> corners = BuildAxisAlignedBoxCorners(boundsMin, boundsMax);
    const float marginFraction = margin / std::max(glm::length(boundsMax - boundsMin), 1.0f);
    const std::vector<glm::vec3> cornersVector(corners.begin(), corners.end());
    return BuildShadowLightSpaceFromWorldPoints(
        lightDirectionTowardSource,
        cornersVector,
        cornersVector,
        shadowMapResolution,
        std::max(marginFraction, 0.02f),
        std::max(marginFraction * 2.0f, 0.08f),
        nullptr,
        nullptr,
        true);
}

ShadowLightSpaceSetup BuildShadowLightSpaceForFrustumCorners(
    const glm::vec3& lightDirectionTowardSource,
    const std::array<glm::vec3, 8>& frustumCorners,
    const int shadowMapResolution,
    const float xyMarginFraction,
    const float zMarginFraction,
    const glm::vec3* casterBoundsMin,
    const glm::vec3* casterBoundsMax,
    const bool tightNearPlaneXyFit,
    const glm::vec3* lightViewAnchor,
    float* stableOrthoHalfExtentInOut,
    const bool allowOrthoShrink)
{
    std::vector<glm::vec3> zPoints(frustumCorners.begin(), frustumCorners.end());
    std::vector<glm::vec3> xyPoints(frustumCorners.begin(), frustumCorners.end());

    if (casterBoundsMin != nullptr && casterBoundsMax != nullptr)
    {
        const std::array<glm::vec3, 8> casterCorners =
            BuildAxisAlignedBoxCorners(*casterBoundsMin, *casterBoundsMax);
        zPoints.insert(zPoints.end(), casterCorners.begin(), casterCorners.end());
        if (!tightNearPlaneXyFit)
        {
            xyPoints.insert(xyPoints.end(), casterCorners.begin(), casterCorners.end());
        }
    }

    return BuildShadowLightSpaceFromWorldPoints(
        lightDirectionTowardSource,
        xyPoints,
        zPoints,
        shadowMapResolution,
        xyMarginFraction,
        zMarginFraction,
        lightViewAnchor,
        stableOrthoHalfExtentInOut,
        allowOrthoShrink);
}

glm::vec3 WorldToShadowNdc(const glm::mat4& lightSpaceMatrix, const glm::vec3& worldPosition)
{
    const glm::vec4 lightSpace = lightSpaceMatrix * glm::vec4(worldPosition, 1.0f);
    const glm::vec3 projected = glm::vec3(lightSpace) / lightSpace.w;
    return projected * 0.5f + 0.5f;
}

float ComputeShadowBias(const float nDotL, const float texelSpan)
{
    const float clampedNDotL = std::clamp(nDotL, 0.0f, 1.0f);
    const float sinTheta = std::sqrt(std::max(1.0f - clampedNDotL * clampedNDotL, 1e-5f));
    return texelSpan * (1.5f + 3.5f * sinTheta / std::max(clampedNDotL, 0.1f));
}

std::vector<float> ComputeCascadeSplitDistances(
    const int cascadeCount,
    const float nearPlane,
    const float farPlane,
    const float splitLambda)
{
    std::vector<float> splits(static_cast<std::size_t>(cascadeCount) + 1U);
    splits[0] = nearPlane;
    splits[static_cast<std::size_t>(cascadeCount)] = farPlane;

    for (int cascadeIndex = 1; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        const float ratio = static_cast<float>(cascadeIndex) / static_cast<float>(cascadeCount);
        const float logSplit = nearPlane * std::pow(farPlane / nearPlane, ratio);
        const float uniformSplit = nearPlane + (farPlane - nearPlane) * ratio;
        splits[static_cast<std::size_t>(cascadeIndex)] =
            splitLambda * logSplit + (1.0f - splitLambda) * uniformSplit;
    }

    return splits;
}

std::array<glm::vec3, 8> ComputeCascadeFrustumCorners(
    const glm::mat4& inverseViewMatrix,
    const float aspect,
    const float fovDegrees,
    const float nearPlane,
    const float farPlane)
{
    const float tanHalfFovY = std::tan(glm::radians(fovDegrees) * 0.5f);
    const float tanHalfFovX = tanHalfFovY * aspect;

    const std::array<glm::vec3, 4> nearPlaneOffsets = {
        glm::vec3(-tanHalfFovX * nearPlane, -tanHalfFovY * nearPlane, -nearPlane),
        glm::vec3(tanHalfFovX * nearPlane, -tanHalfFovY * nearPlane, -nearPlane),
        glm::vec3(-tanHalfFovX * nearPlane, tanHalfFovY * nearPlane, -nearPlane),
        glm::vec3(tanHalfFovX * nearPlane, tanHalfFovY * nearPlane, -nearPlane),
    };

    const std::array<glm::vec3, 4> farPlaneOffsets = {
        glm::vec3(-tanHalfFovX * farPlane, -tanHalfFovY * farPlane, -farPlane),
        glm::vec3(tanHalfFovX * farPlane, -tanHalfFovY * farPlane, -farPlane),
        glm::vec3(-tanHalfFovX * farPlane, tanHalfFovY * farPlane, -farPlane),
        glm::vec3(tanHalfFovX * farPlane, tanHalfFovY * farPlane, -farPlane),
    };

    std::array<glm::vec3, 8> corners{};
    for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
    {
        const glm::vec4 nearWorld = inverseViewMatrix * glm::vec4(nearPlaneOffsets[cornerIndex], 1.0f);
        const glm::vec4 farWorld = inverseViewMatrix * glm::vec4(farPlaneOffsets[cornerIndex], 1.0f);
        corners[static_cast<std::size_t>(cornerIndex)] = glm::vec3(nearWorld) / nearWorld.w;
        corners[static_cast<std::size_t>(cornerIndex + 4)] = glm::vec3(farWorld) / farWorld.w;
    }

    return corners;
}

glm::vec3 ComputeBoundsMin(const std::array<glm::vec3, 8>& points)
{
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    for (const glm::vec3& point : points)
    {
        boundsMin = glm::min(boundsMin, point);
    }
    return boundsMin;
}

glm::vec3 ComputeBoundsMax(const std::array<glm::vec3, 8>& points)
{
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& point : points)
    {
        boundsMax = glm::max(boundsMax, point);
    }
    return boundsMax;
}

bool ComputeBoundsIntersection(
    const glm::vec3& boundsAMin,
    const glm::vec3& boundsAMax,
    const glm::vec3& boundsBMin,
    const glm::vec3& boundsBMax,
    glm::vec3& intersectionMin,
    glm::vec3& intersectionMax)
{
    intersectionMin = glm::max(boundsAMin, boundsBMin);
    intersectionMax = glm::min(boundsAMax, boundsBMax);

    return intersectionMin.x <= intersectionMax.x &&
        intersectionMin.y <= intersectionMax.y &&
        intersectionMin.z <= intersectionMax.z;
}

float ComputeShadowDrawDistance(
    const glm::vec3& cameraPosition,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const float margin)
{
    const std::array<glm::vec3, 8> corners = {
        glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
        glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
        glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
        glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
        glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
        glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
        glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
        glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
    };

    float maxDistance = 0.0f;
    for (const glm::vec3& corner : corners)
    {
        maxDistance = std::max(maxDistance, glm::length(corner - cameraPosition));
    }

    return maxDistance + margin;
}
