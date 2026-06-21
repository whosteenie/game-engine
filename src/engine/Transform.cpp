#include "engine/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

glm::mat4 Transform::ToMatrix(double animationTime, bool autoSpin) const
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, position);

    if (autoSpin)
    {
        model = glm::rotate(model, static_cast<float>(animationTime) * 1.5f, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, static_cast<float>(animationTime) * 0.6f, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    else
    {
        model = glm::rotate(model, glm::radians(rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    model = glm::scale(model, scale);
    return model;
}
