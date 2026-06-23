#include "engine/components/LightComponent.h"

#include "engine/components/ComponentCompare.h"
#include "engine/scene/JsonMath.h"
#include "engine/scene/RotationUtils.h"
#include "engine/scene/SceneObject.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <cmath>

using json = nlohmann::json;

bool operator==(const LightComponent& left, const LightComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return left.type == right.type
        && left.color == right.color
        && FloatsEqual(left.intensity, right.intensity)
        && FloatsEqual(left.constantAttenuation, right.constantAttenuation)
        && FloatsEqual(left.linearAttenuation, right.linearAttenuation)
        && FloatsEqual(left.quadraticAttenuation, right.quadraticAttenuation)
        && FloatsEqual(left.range, right.range)
        && FloatsEqual(left.innerCutoffDegrees, right.innerCutoffDegrees)
        && FloatsEqual(left.outerCutoffDegrees, right.outerCutoffDegrees)
        && left.castsShadow == right.castsShadow;
}

json LightComponentToJson(const LightComponent& light)
{
    return json{
        {"type", LightTypeToString(light.type)},
        {"color", Vec3ToJson(light.color)},
        {"intensity", light.intensity},
        {"constantAttenuation", light.constantAttenuation},
        {"linearAttenuation", light.linearAttenuation},
        {"quadraticAttenuation", light.quadraticAttenuation},
        {"range", light.range},
        {"innerCutoffDegrees", light.innerCutoffDegrees},
        {"outerCutoffDegrees", light.outerCutoffDegrees},
        {"castsShadow", light.castsShadow},
    };
}

LightComponent LightComponentFromJson(const json& value)
{
    LightType type = LightType::Point;
    if (value.contains("type"))
    {
        LightTypeFromString(value.at("type").get<std::string>(), type);
    }

    LightComponent light = MakeDefaultLightComponent(type);
    light.color = Vec3FromJson(value.at("color"));
    light.intensity = value.at("intensity").get<float>();
    light.constantAttenuation = value.value("constantAttenuation", light.constantAttenuation);
    light.linearAttenuation = value.value("linearAttenuation", light.linearAttenuation);
    light.quadraticAttenuation = value.value("quadraticAttenuation", light.quadraticAttenuation);
    light.range = value.value("range", light.range);
    light.innerCutoffDegrees = value.value("innerCutoffDegrees", light.innerCutoffDegrees);
    light.outerCutoffDegrees = value.value("outerCutoffDegrees", light.outerCutoffDegrees);
    light.castsShadow = value.value("castsShadow", light.castsShadow);
    return light;
}

LightComponent MakeDefaultLightComponent(LightType type)
{
    LightComponent component;
    component.type = type;

    switch (type)
    {
    case LightType::Directional:
        component.color = glm::vec3(1.0f, 0.97f, 0.92f);
        component.intensity = 2.5f;
        component.castsShadow = true;
        break;
    case LightType::Point:
        component.color = glm::vec3(1.0f, 0.85f, 0.65f);
        component.intensity = 2.0f;
        component.range = 10.0f;
        break;
    case LightType::Spot:
        component.color = glm::vec3(0.75f, 0.9f, 1.0f);
        component.intensity = 3.0f;
        component.range = 8.0f;
        component.innerCutoffDegrees = 10.0f;
        component.outerCutoffDegrees = 18.0f;
        break;
    }

    return component;
}

Transform MakeDefaultLightTransform(LightType type)
{
    Transform transform;

    switch (type)
    {
    case LightType::Directional:
        transform.position = glm::vec3(0.0f, 6.0f, 0.0f);
        transform.rotation = RotationUtils::QuatFromLocalYAxis(glm::normalize(glm::vec3(0.45f, 0.7f, 0.55f)));
        break;
    case LightType::Point:
        transform.position = glm::vec3(2.0f, 3.0f, 1.0f);
        break;
    case LightType::Spot:
        transform.position = glm::vec3(-2.5f, 4.0f, 0.5f);
        transform.rotation = RotationUtils::QuatFromLocalYAxis(glm::normalize(glm::vec3(0.2f, -1.0f, -0.1f)));
        break;
    }

    return transform;
}

Light BuildLightFromSceneObject(const SceneObject& object, const glm::mat4& worldMatrix)
{
    const LightComponent& component = object.GetLight();
    const glm::vec3 worldPosition = glm::vec3(worldMatrix[3]);
    const glm::vec3 towardLight = RotationUtils::ExtractLocalYWorldDirection(worldMatrix);

    Light light = Light::MakePoint(worldPosition, component.color, component.intensity);

    switch (component.type)
    {
    case LightType::Directional:
        light = Light::MakeDirectional(towardLight, component.color, component.intensity);
        light.SetPosition(worldPosition);
        break;
    case LightType::Point:
        light = Light::MakePoint(
            worldPosition,
            component.color,
            component.intensity,
            component.constantAttenuation,
            component.linearAttenuation,
            component.quadraticAttenuation,
            component.range);
        break;
    case LightType::Spot:
        light = Light::MakeSpot(
            worldPosition,
            towardLight,
            component.color,
            component.intensity,
            component.innerCutoffDegrees,
            component.outerCutoffDegrees,
            component.constantAttenuation,
            component.linearAttenuation,
            component.quadraticAttenuation,
            component.range);
        break;
    }

    return light;
}
