#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace RotationUtils
{
    glm::vec3 NormalizeOrFallback(const glm::vec3& vector, const glm::vec3& fallback);

    glm::quat QuatFromLocalYAxis(const glm::vec3& localYWorldDirection);
    glm::quat QuatFromLocalNegativeZAxis(const glm::vec3& negativeZWorldDirection);

    glm::vec3 ExtractLocalYWorldDirection(const glm::mat4& worldMatrix);

    void ExtractCameraBasis(
        const glm::mat4& worldMatrix,
        glm::vec3& outPosition,
        glm::vec3& outForward,
        glm::vec3& outUp);
}
