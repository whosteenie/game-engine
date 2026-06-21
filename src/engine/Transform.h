#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct Transform
{
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    glm::mat4 ToMatrix() const;
    void SetFromMatrix(const glm::mat4& matrix);

    glm::vec3 GetRotationDegrees() const;
    void SetRotationDegrees(const glm::vec3& eulerDegrees);

    void Reset();
};
