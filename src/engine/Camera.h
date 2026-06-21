#pragma once

#include <glm/glm.hpp>

class Camera
{
public:
    Camera(
        const glm::vec3& position,
        const glm::vec3& target,
        const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    void SetAspectFromFramebuffer(int width, int height);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;
    glm::vec3 GetPosition() const;

private:
    glm::vec3 m_position;
    glm::vec3 m_target;
    glm::vec3 m_up;

    float m_fov = 45.0f;
    float m_aspect = 1.0f;
    float m_near = 0.1f;
    float m_far = 100.0f;
};
