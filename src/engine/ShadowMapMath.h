#pragma once

#include <array>
#include <glm/glm.hpp>
#include <vector>

struct ShadowLightSpaceSetup
{
    glm::mat4 lightSpaceMatrix{1.0f};
    glm::mat4 lightView{1.0f};
    glm::mat4 lightProjection{1.0f};
    glm::vec2 snapOffsetNdc{0.0f};
    glm::vec3 sceneCenter{0.0f};
    float orthoWidth = 0.0f;
    float orthoHeight = 0.0f;
    float texelWorldSizeX = 0.0f;
    float texelWorldSizeY = 0.0f;
};

ShadowLightSpaceSetup BuildShadowLightSpace(
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    int shadowMapResolution,
    float margin = 1.0f);

ShadowLightSpaceSetup BuildShadowLightSpaceForFrustumCorners(
    const glm::vec3& lightDirectionTowardSource,
    const std::array<glm::vec3, 8>& frustumCorners,
    int shadowMapResolution,
    float xyMarginFraction,
    float zMarginFraction,
    const glm::vec3* casterBoundsMin = nullptr,
    const glm::vec3* casterBoundsMax = nullptr,
    bool tightNearPlaneXyFit = true,
    const glm::vec3* lightViewAnchor = nullptr,
    float* stableOrthoHalfExtentInOut = nullptr,
    bool allowOrthoShrink = true);

glm::vec3 WorldToShadowNdc(const glm::mat4& lightSpaceMatrix, const glm::vec3& worldPosition);

float ComputeShadowBias(float nDotL, float texelSpan);

std::vector<float> ComputeCascadeSplitDistances(
    int cascadeCount,
    float nearPlane,
    float farPlane,
    float splitLambda = 0.5f);

std::array<glm::vec3, 8> ComputeCascadeFrustumCorners(
    const glm::mat4& inverseViewMatrix,
    float aspect,
    float fovDegrees,
    float nearPlane,
    float farPlane);

glm::vec3 ComputeBoundsMin(const std::array<glm::vec3, 8>& points);

glm::vec3 ComputeBoundsMax(const std::array<glm::vec3, 8>& points);

bool ComputeBoundsIntersection(
    const glm::vec3& boundsAMin,
    const glm::vec3& boundsAMax,
    const glm::vec3& boundsBMin,
    const glm::vec3& boundsBMax,
    glm::vec3& intersectionMin,
    glm::vec3& intersectionMax);

float ComputeShadowDrawDistance(
    const glm::vec3& cameraPosition,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    float margin = 4.0f);
