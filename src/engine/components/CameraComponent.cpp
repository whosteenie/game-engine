#include "engine/components/CameraComponent.h"

#include "engine/components/ComponentCompare.h"
#include "engine/scene/JsonMath.h"
#include "engine/scene/RotationUtils.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>

using json = nlohmann::json;

namespace
{
    glm::vec3 DirectionFromYawPitch(float yawDegrees, float pitchDegrees)
    {
        glm::vec3 direction;
        direction.x = cos(glm::radians(yawDegrees)) * cos(glm::radians(pitchDegrees));
        direction.y = sin(glm::radians(pitchDegrees));
        direction.z = sin(glm::radians(yawDegrees)) * cos(glm::radians(pitchDegrees));
        return glm::normalize(direction);
    }
}

bool operator==(const CameraComponent& left, const CameraComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return FloatsEqual(left.fovDegrees, right.fovDegrees)
        && FloatsEqual(left.nearPlane, right.nearPlane)
        && FloatsEqual(left.farPlane, right.farPlane)
        && left.enabled == right.enabled
        && left.depth == right.depth
        && left.isMain == right.isMain;
}

json CameraComponentToJson(const CameraComponent& camera)
{
    return json{
        {"fovDegrees", camera.fovDegrees},
        {"nearPlane", camera.nearPlane},
        {"farPlane", camera.farPlane},
        {"enabled", camera.enabled},
        {"depth", camera.depth},
        {"isMain", camera.isMain},
    };
}

CameraComponent CameraComponentFromJson(const json& value)
{
    CameraComponent camera = MakeDefaultCameraComponent();
    camera.fovDegrees = value.value("fovDegrees", camera.fovDegrees);
    camera.nearPlane = value.value("nearPlane", camera.nearPlane);
    camera.farPlane = value.value("farPlane", camera.farPlane);
    camera.enabled = value.value("enabled", camera.enabled);
    camera.depth = value.value("depth", camera.depth);
    camera.isMain = value.value("isMain", camera.isMain);
    return camera;
}

CameraComponent MakeDefaultCameraComponent()
{
    return CameraComponent{};
}

Transform MakeDefaultCameraTransform()
{
    Transform transform;
    transform.position = glm::vec3(6.0f, 5.0f, 6.0f);
    transform.rotation = RotationUtils::QuatFromLocalNegativeZAxis(DirectionFromYawPitch(-135.0f, -35.0f));
    return transform;
}
