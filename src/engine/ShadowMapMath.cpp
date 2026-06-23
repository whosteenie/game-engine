#include "engine/ShadowMapMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

ShadowLightSpaceSetup BuildShadowLightSpace(
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const int shadowMapResolution,
    const float margin)
{
    ShadowLightSpaceSetup setup;
    setup.sceneCenter = (boundsMin + boundsMax) * 0.5f;

    const glm::vec3 normalizedLightDirection = glm::normalize(lightDirectionTowardSource);
    const float boundsDepth = glm::length(boundsMax - boundsMin);
    const float lightDistance = boundsDepth * 0.5f + 12.0f;
    const glm::vec3 lightEye = setup.sceneCenter + normalizedLightDirection * lightDistance;
    setup.lightView = glm::lookAt(lightEye, setup.sceneCenter, glm::vec3(0.0f, 1.0f, 0.0f));

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

    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (const glm::vec3& corner : corners)
    {
        const glm::vec4 lightSpaceCorner = setup.lightView * glm::vec4(corner, 1.0f);
        minX = std::min(minX, lightSpaceCorner.x);
        maxX = std::max(maxX, lightSpaceCorner.x);
        minY = std::min(minY, lightSpaceCorner.y);
        maxY = std::max(maxY, lightSpaceCorner.y);
        minZ = std::min(minZ, lightSpaceCorner.z);
        maxZ = std::max(maxZ, lightSpaceCorner.z);
    }

    setup.orthoWidth = (maxX - minX) + margin * 2.0f;
    setup.orthoHeight = (maxY - minY) + margin * 2.0f;
    setup.texelWorldSizeX = setup.orthoWidth / static_cast<float>(shadowMapResolution);
    setup.texelWorldSizeY = setup.orthoHeight / static_cast<float>(shadowMapResolution);

    setup.lightProjection = glm::ortho(
        minX - margin,
        maxX + margin,
        minY - margin,
        maxY + margin,
        -maxZ - margin,
        -minZ + margin);

    setup.lightSpaceMatrix = setup.lightProjection * setup.lightView;

    glm::vec4 shadowOrigin = setup.lightSpaceMatrix * glm::vec4(setup.sceneCenter, 1.0f);
    const float resolutionScale = static_cast<float>(shadowMapResolution) * 0.5f;
    shadowOrigin *= resolutionScale;
    const glm::vec4 roundedOrigin = glm::round(shadowOrigin);
    const glm::vec4 roundOffset =
        (roundedOrigin - shadowOrigin) * (2.0f / static_cast<float>(shadowMapResolution));

    setup.snapOffsetNdc = glm::vec2(roundOffset.x, roundOffset.y);

    glm::mat4 snapMatrix(1.0f);
    snapMatrix[3][0] = roundOffset.x;
    snapMatrix[3][1] = roundOffset.y;
    setup.lightSpaceMatrix = snapMatrix * setup.lightSpaceMatrix;

    return setup;
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
