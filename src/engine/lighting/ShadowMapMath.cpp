#include "engine/lighting/ShadowMapMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    // Fixed world anchor (not the camera): same orientation as the old per-camera lookAt, but
    // translation stays constant so shadow texels do not swim when the view moves.
    glm::mat4 BuildStableLightViewMatrix(const glm::vec3& lightDirectionTowardSource)
    {
        const glm::vec3 normalizedLightDirection = glm::normalize(lightDirectionTowardSource);
        const glm::vec3 viewTarget = glm::vec3(0.0f);
        const glm::vec3 lightEye = viewTarget + normalizedLightDirection * 50.0f;

        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(normalizedLightDirection, up)) > 0.99f)
        {
            up = glm::vec3(0.0f, 0.0f, 1.0f);
        }

        return glm::lookAtLH(lightEye, viewTarget, up);
    }

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

    void AccumulateClipDepthRange(
        ShadowLightSpaceSetup& setup,
        const std::vector<glm::vec3>& worldPoints)
    {
        setup.clipDepthContentMin = 1.0f;
        setup.clipDepthContentMax = 0.0f;
        for (const glm::vec3& point : worldPoints)
        {
            const glm::vec4 clip = setup.lightSpaceMatrix * glm::vec4(point, 1.0f);
            const float clipZ = clip.z / clip.w;
            setup.clipDepthContentMin = std::min(setup.clipDepthContentMin, clipZ);
            setup.clipDepthContentMax = std::max(setup.clipDepthContentMax, clipZ);
        }

        const float clipSpan = std::max(setup.clipDepthContentMax - setup.clipDepthContentMin, 1e-4f);
        setup.clipDepthContentMin =
            std::clamp(setup.clipDepthContentMin - clipSpan * 0.02f, 0.0f, 1.0f);
        setup.clipDepthContentMax =
            std::clamp(setup.clipDepthContentMax + clipSpan * 0.02f, 0.0f, 1.0f);
        if (setup.clipDepthContentMax <= setup.clipDepthContentMin)
        {
            setup.clipDepthContentMin = 0.0f;
            setup.clipDepthContentMax = 1.0f;
        }
    }

    glm::vec2 SnapLightSpaceCenterToWorldStableTexelGrid(
        const float centerX,
        const float centerY,
        const glm::mat4& lightView,
        const float texelWorldSize)
    {
        const glm::vec4 worldOriginLightSpace = lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const float anchorX = worldOriginLightSpace.x;
        const float anchorY = worldOriginLightSpace.y;

        const float snappedCenterX = anchorX +
            std::floor((centerX - anchorX) / texelWorldSize) * texelWorldSize + texelWorldSize * 0.5f;
        const float snappedCenterY = anchorY +
            std::floor((centerY - anchorY) / texelWorldSize) * texelWorldSize + texelWorldSize * 0.5f;
        return glm::vec2(snappedCenterX, snappedCenterY);
    }

    ShadowLightSpaceSetup BuildShadowLightSpaceFromWorldPoints(
        const glm::vec3& lightDirectionTowardSource,
        const std::vector<glm::vec3>& xyWorldPoints,
        const std::vector<glm::vec3>& zWorldPoints,
        const int shadowMapResolution,
        const float xyMarginFraction,
        const float zMarginFraction)
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

        setup.lightView = BuildStableLightViewMatrix(lightDirectionTowardSource);

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
        const float marginX = std::max(spanX * xyMarginFraction, 1e-4f);
        const float marginY = std::max(spanY * xyMarginFraction, 1e-4f);
        const float marginZ = std::max(spanZ * zMarginFraction, 1e-4f);

        float centerX = (minX + maxX) * 0.5f;
        float centerY = (minY + maxY) * 0.5f;
        float halfExtent = std::max(spanX * 0.5f + marginX, spanY * 0.5f + marginY);

        float texelWorldSize = (halfExtent * 2.0f) / static_cast<float>(shadowMapResolution);
        halfExtent = std::ceil(halfExtent / texelWorldSize) * texelWorldSize;
        texelWorldSize = (halfExtent * 2.0f) / static_cast<float>(shadowMapResolution);

        const glm::vec2 snappedCenter = SnapLightSpaceCenterToWorldStableTexelGrid(
            centerX,
            centerY,
            setup.lightView,
            texelWorldSize);
        const float snappedCenterX = snappedCenter.x;
        const float snappedCenterY = snappedCenter.y;

        const float orthoNear = minZ - marginZ;
        const float orthoFar = maxZ + marginZ;
        setup.contentOrthoNear = orthoNear;
        setup.contentOrthoFar = orthoFar;
        setup.stableOrthoNear = orthoNear;
        setup.stableOrthoFar = orthoFar;

        setup.orthoWidth = halfExtent * 2.0f;
        setup.orthoHeight = halfExtent * 2.0f;
        setup.texelWorldSizeX = texelWorldSize;
        setup.texelWorldSizeY = texelWorldSize;

        setup.lightProjection = glm::orthoLH_ZO(
            snappedCenterX - halfExtent,
            snappedCenterX + halfExtent,
            snappedCenterY - halfExtent,
            snappedCenterY + halfExtent,
            orthoNear,
            orthoFar);

        setup.lightSpaceMatrix = setup.lightProjection * setup.lightView;
        if (halfExtent > 1e-6f)
        {
            setup.snapOffsetNdc = glm::vec2(
                (snappedCenterX - centerX) / halfExtent,
                (snappedCenterY - centerY) / halfExtent);
        }
        else
        {
            setup.snapOffsetNdc = glm::vec2(0.0f);
        }

        AccumulateClipDepthRange(setup, zWorldPoints);

        return setup;
    }

    void ApplyFrustumContentDepthRange(
        ShadowLightSpaceSetup& setup,
        const std::array<glm::vec3, 8>& frustumCorners,
        const float /*zMarginFraction*/)
    {
        AccumulateClipDepthRange(
            setup,
            std::vector<glm::vec3>(frustumCorners.begin(), frustumCorners.end()));
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
        std::max(marginFraction * 2.0f, 0.08f));
}

ShadowLightSpaceSetup BuildShadowLightSpaceForFrustumCorners(
    const glm::vec3& lightDirectionTowardSource,
    const std::array<glm::vec3, 8>& frustumCorners,
    const int shadowMapResolution,
    const float xyMarginFraction,
    const float zMarginFraction,
    const glm::vec3* /*casterBoundsMin*/,
    const glm::vec3* /*casterBoundsMax*/)
{
    const std::vector<glm::vec3> frustumPoints(frustumCorners.begin(), frustumCorners.end());

    ShadowLightSpaceSetup setup = BuildShadowLightSpaceFromWorldPoints(
        lightDirectionTowardSource,
        frustumPoints,
        frustumPoints,
        shadowMapResolution,
        xyMarginFraction,
        zMarginFraction);

    ApplyFrustumContentDepthRange(setup, frustumCorners, zMarginFraction);
    return setup;
}

ShadowLightSpaceSetup BuildShadowLightSpaceForBounds(
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const int shadowMapResolution,
    const float xyMarginFraction,
    const float zMarginFraction)
{
    const std::array<glm::vec3, 8> corners = BuildAxisAlignedBoxCorners(boundsMin, boundsMax);
    const std::vector<glm::vec3> boundsPoints(corners.begin(), corners.end());
    return BuildShadowLightSpaceFromWorldPoints(
        lightDirectionTowardSource,
        boundsPoints,
        boundsPoints,
        shadowMapResolution,
        xyMarginFraction,
        zMarginFraction);
}

glm::vec3 WorldToShadowNdc(const glm::mat4& lightSpaceMatrix, const glm::vec3& worldPosition)
{
    const glm::vec4 lightSpace = lightSpaceMatrix * glm::vec4(worldPosition, 1.0f);
    const glm::vec3 projected = glm::vec3(lightSpace) / lightSpace.w;
    // orthoLH_ZO clip xy are [-1, 1]; depth is already in [0, 1].
    return glm::vec3(projected.x * 0.5f + 0.5f, projected.y * 0.5f + 0.5f, projected.z);
}

glm::vec3 WorldToShadowSampleCoords(const glm::mat4& lightSpaceMatrix, const glm::vec3& worldPosition)
{
    const glm::vec4 lightSpace = lightSpaceMatrix * glm::vec4(worldPosition, 1.0f);
    glm::vec3 coords = glm::vec3(lightSpace) / lightSpace.w;
    coords.x = coords.x * 0.5f + 0.5f;
    coords.y = coords.y * 0.5f + 0.5f;
    coords.y = 1.0f - coords.y;
    return coords;
}

namespace
{
    bool IsInShadowCascadeBounds(const glm::vec3& sampleCoords)
    {
        return sampleCoords.z >= 0.0f && sampleCoords.z <= 1.0f && sampleCoords.x >= 0.0f
            && sampleCoords.x <= 1.0f && sampleCoords.y >= 0.0f && sampleCoords.y <= 1.0f;
    }

    int SelectCascadeIndexFromViewDepth(const float viewDepth, const float* cascadeEndSplits, const int cascadeCount)
    {
        int cascadeIndex = 0;
        for (int splitIndex = 0; splitIndex < cascadeCount - 1; ++splitIndex)
        {
            if (viewDepth > cascadeEndSplits[splitIndex])
            {
                cascadeIndex = splitIndex + 1;
            }
        }
        return cascadeIndex;
    }

    float StableClipZToContentClipZ(
        const ShadowLightSpaceSetup& setup,
        const float stableClipZ)
    {
        const float depthRange = std::max(setup.clipDepthContentMax - setup.clipDepthContentMin, 1e-5f);
        return (stableClipZ - setup.clipDepthContentMin) / depthRange;
    }

    float NormalizeCascadeClipDepth(
        const ShadowLightSpaceSetup& setup,
        const float stableClipZ)
    {
        return std::clamp(StableClipZToContentClipZ(setup, stableClipZ), 0.0f, 1.0f);
    }
}

ShadowReceiverProbeResult EvaluateShadowReceiverProbe(
    const glm::vec3& worldPosition,
    const float viewDepth,
    const glm::mat4* lightSpaceMatrices,
    const ShadowLightSpaceSetup* cascadeSetups,
    const float* cascadeEndSplits,
    const int cascadeCount)
{
    ShadowReceiverProbeResult result;
    if (lightSpaceMatrices == nullptr || cascadeSetups == nullptr || cascadeCount <= 0)
    {
        return result;
    }

    int cascadeIndex = 0;
    bool foundContainingCascade = false;
    for (int candidateIndex = 0; candidateIndex < cascadeCount; ++candidateIndex)
    {
        const glm::vec3 sampleCoords =
            WorldToShadowSampleCoords(lightSpaceMatrices[candidateIndex], worldPosition);
        if (IsInShadowCascadeBounds(sampleCoords))
        {
            cascadeIndex = candidateIndex;
            foundContainingCascade = true;
            break;
        }
    }

    if (!foundContainingCascade && cascadeEndSplits != nullptr)
    {
        cascadeIndex = SelectCascadeIndexFromViewDepth(viewDepth, cascadeEndSplits, cascadeCount);
        cascadeIndex = std::clamp(cascadeIndex, 0, cascadeCount - 1);
    }

    const glm::vec3 sampleCoords =
        WorldToShadowSampleCoords(lightSpaceMatrices[cascadeIndex], worldPosition);
    result.cascadeIndex = cascadeIndex;
    result.shadowUv = glm::vec2(sampleCoords.x, sampleCoords.y);
    result.receiverClipZ = sampleCoords.z;
    result.inBounds = IsInShadowCascadeBounds(sampleCoords);
    result.normalizedClipZ =
        NormalizeCascadeClipDepth(cascadeSetups[cascadeIndex], sampleCoords.z);
    return result;
}

float ComputeShadowBias(const float nDotL, const float texelSpan)
{
    const float clampedNDotL = std::clamp(nDotL, 0.0f, 1.0f);
    const float sinTheta = std::sqrt(std::max(1.0f - clampedNDotL * clampedNDotL, 1e-5f));
    return texelSpan * (1.5f + 3.5f * sinTheta / std::max(clampedNDotL, 0.1f));
}

float ComputeCasterDepthBiasNormalized(
    const float texelWorldSize,
    const float orthoNear,
    const float orthoFar,
    const float scale)
{
    const float depthSpan = std::max(orthoFar - orthoNear, 1e-3f);
    return (texelWorldSize / depthSpan) * std::max(scale, 0.0f);
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
        glm::vec3(-tanHalfFovX * nearPlane, -tanHalfFovY * nearPlane, nearPlane),
        glm::vec3(tanHalfFovX * nearPlane, -tanHalfFovY * nearPlane, nearPlane),
        glm::vec3(-tanHalfFovX * nearPlane, tanHalfFovY * nearPlane, nearPlane),
        glm::vec3(tanHalfFovX * nearPlane, tanHalfFovY * nearPlane, nearPlane),
    };

    const std::array<glm::vec3, 4> farPlaneOffsets = {
        glm::vec3(-tanHalfFovX * farPlane, -tanHalfFovY * farPlane, farPlane),
        glm::vec3(tanHalfFovX * farPlane, -tanHalfFovY * farPlane, farPlane),
        glm::vec3(-tanHalfFovX * farPlane, tanHalfFovY * farPlane, farPlane),
        glm::vec3(tanHalfFovX * farPlane, tanHalfFovY * farPlane, farPlane),
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
