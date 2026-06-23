#include "engine/CameraComponent.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace
{
    glm::vec3 NormalizeOrFallback(const glm::vec3& vector, const glm::vec3& fallback)
    {
        const float length = glm::length(vector);
        if (length < 0.0001f)
        {
            return fallback;
        }

        return vector / length;
    }

    glm::quat QuatFromLocalNegativeZAxis(const glm::vec3& negativeZWorldDirection)
    {
        const glm::vec3 zAxis =
            NormalizeOrFallback(negativeZWorldDirection, glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 reference =
            glm::abs(zAxis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 xAxis =
            NormalizeOrFallback(glm::cross(reference, zAxis), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 yAxis = glm::cross(zAxis, xAxis);
        const glm::mat3 rotationMatrix(xAxis, yAxis, zAxis);
        return glm::quat_cast(rotationMatrix);
    }

    glm::vec3 DirectionFromYawPitch(float yawDegrees, float pitchDegrees)
    {
        glm::vec3 direction;
        direction.x = cos(glm::radians(yawDegrees)) * cos(glm::radians(pitchDegrees));
        direction.y = sin(glm::radians(pitchDegrees));
        direction.z = sin(glm::radians(yawDegrees)) * cos(glm::radians(pitchDegrees));
        return glm::normalize(direction);
    }
}

CameraComponent MakeDefaultCameraComponent()
{
    return CameraComponent{};
}

Transform MakeDefaultCameraTransform()
{
    Transform transform;
    transform.position = glm::vec3(6.0f, 5.0f, 6.0f);
    transform.rotation = QuatFromLocalNegativeZAxis(DirectionFromYawPitch(-135.0f, -35.0f));
    return transform;
}
