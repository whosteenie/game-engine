#include "engine/scene/Transform.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cfloat>
#include <limits>

namespace
{
    glm::vec3 SanitizeScale(const glm::vec3& scale)
    {
        glm::vec3 sanitizedScale = scale;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(sanitizedScale[axis]) < FLT_EPSILON)
            {
                sanitizedScale[axis] = 0.001f;
            }
        }

        return sanitizedScale;
    }
}

glm::mat4 Transform::ToMatrix() const
{
    const glm::vec3 safeScale = SanitizeScale(scale);

    glm::mat4 matrix(1.0f);
    matrix = glm::translate(matrix, position);
    matrix *= glm::mat4_cast(glm::normalize(rotation));
    matrix = glm::scale(matrix, safeScale);
    return matrix;
}

void Transform::SetFromMatrix(const glm::mat4& matrix)
{
    position = glm::vec3(matrix[3]);

    const glm::vec3 column0 = glm::vec3(matrix[0]);
    const glm::vec3 column1 = glm::vec3(matrix[1]);
    const glm::vec3 column2 = glm::vec3(matrix[2]);

    scale.x = glm::length(column0);
    scale.y = glm::length(column1);
    scale.z = glm::length(column2);

    glm::mat3 rotationMatrix(1.0f);
    const float epsilon = std::numeric_limits<float>::epsilon();

    if (scale.x > epsilon)
    {
        rotationMatrix[0] = column0 / scale.x;
    }

    if (scale.y > epsilon)
    {
        rotationMatrix[1] = column1 / scale.y;
    }

    if (scale.z > epsilon)
    {
        rotationMatrix[2] = column2 / scale.z;
    }

    rotation = glm::normalize(glm::quat_cast(rotationMatrix));
}

glm::vec3 Transform::GetRotationDegrees() const
{
    return glm::degrees(glm::eulerAngles(rotation));
}

void Transform::SetRotationDegrees(const glm::vec3& eulerDegrees)
{
    rotation = glm::normalize(glm::quat(glm::radians(eulerDegrees)));
}

void Transform::Reset()
{
    position = glm::vec3(0.0f);
    rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    scale = glm::vec3(1.0f);
}
