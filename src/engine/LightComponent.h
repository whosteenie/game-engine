#pragma once

#include "engine/Light.h"
#include "engine/Transform.h"

#include <glm/glm.hpp>
#include <optional>

class SceneObject;

struct LightComponent
{
    LightType type = LightType::Point;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float constantAttenuation = 1.0f;
    float linearAttenuation = 0.07f;
    float quadraticAttenuation = 0.017f;
    float range = 10.0f;
    float innerCutoffDegrees = 12.5f;
    float outerCutoffDegrees = 17.5f;
    bool castsShadow = false;
};

glm::quat QuatFromLocalYAxis(const glm::vec3& localYWorldDirection);

LightComponent MakeDefaultLightComponent(LightType type);
Transform MakeDefaultLightTransform(LightType type);

Light BuildLightFromSceneObject(const SceneObject& object, const glm::mat4& worldMatrix);
