#include "engine/scene/RotationUtils.h"

#include "engine/scene/Transform.h"

#include <glm/gtc/constants.hpp>

namespace RotationUtils
{    glm::vec3 NormalizeOrFallback(const glm::vec3& vector, const glm::vec3& fallback)
    {
        const float length = glm::length(vector);
        if (length < 0.0001f)
        {
            return fallback;
        }

        return vector / length;
    }

    glm::quat QuatFromLocalYAxis(const glm::vec3& localYWorldDirection)
    {
        const glm::vec3 yAxis = NormalizeOrFallback(localYWorldDirection, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 reference =
            glm::abs(yAxis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 zAxis = NormalizeOrFallback(glm::cross(reference, yAxis), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 xAxis = glm::cross(yAxis, zAxis);
        const glm::mat3 rotationMatrix(xAxis, yAxis, zAxis);
        return glm::quat_cast(rotationMatrix);
    }

    glm::quat QuatFromLocalNegativeZAxis(const glm::vec3& negativeZWorldDirection)
    {
        const glm::vec3 zAxis =
            NormalizeOrFallback(-negativeZWorldDirection, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 reference =
            glm::abs(zAxis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 xAxis =
            NormalizeOrFallback(glm::cross(reference, zAxis), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 yAxis = glm::cross(zAxis, xAxis);
        const glm::mat3 rotationMatrix(xAxis, yAxis, zAxis);
        return glm::quat_cast(rotationMatrix);
    }

    glm::vec3 ExtractLocalYWorldDirection(const glm::mat4& worldMatrix)
    {
        const glm::mat3 rotationMatrix = glm::mat3(worldMatrix);
        return NormalizeOrFallback(
            rotationMatrix * glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void ExtractCameraBasis(
        const glm::mat4& worldMatrix,
        glm::vec3& outPosition,
        glm::vec3& outForward,
        glm::vec3& outUp)
    {
        const glm::mat3 rotationMatrix = glm::mat3(worldMatrix);
        outPosition = glm::vec3(worldMatrix[3]);
        outForward = NormalizeOrFallback(-glm::vec3(rotationMatrix[2]), glm::vec3(0.0f, 0.0f, -1.0f));
        outUp = NormalizeOrFallback(glm::vec3(rotationMatrix[1]), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 BuildCameraObjectWorldMatrixFromEditorViewInverse(const glm::mat4& inverseViewMatrix)
    {
        Transform transform = Transform::FromMatrix(inverseViewMatrix);
        transform.rotation = glm::normalize(
            transform.rotation * glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)));
        return transform.ToMatrix();
    }
}
