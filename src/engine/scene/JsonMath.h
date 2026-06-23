#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

inline nlohmann::json Vec3ToJson(const glm::vec3& value)
{
    return nlohmann::json::array({value.x, value.y, value.z});
}

inline glm::vec3 Vec3FromJson(const nlohmann::json& value)
{
    return glm::vec3(value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>());
}

inline nlohmann::json QuatToJson(const glm::quat& value)
{
    return nlohmann::json::array({value.w, value.x, value.y, value.z});
}

inline glm::quat QuatFromJson(const nlohmann::json& value)
{
    return glm::quat(value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>(), value.at(3).get<float>());
}
