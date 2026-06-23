#pragma once

#include <glm/glm.hpp>

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

glm::vec3 WorldToShadowNdc(const glm::mat4& lightSpaceMatrix, const glm::vec3& worldPosition);

float ComputeShadowBias(float nDotL, float texelSpan);
