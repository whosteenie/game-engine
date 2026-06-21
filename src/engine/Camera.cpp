#include "engine/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

Camera::Camera(const glm::vec3& position, const glm::vec3& target, const glm::vec3& up)
    : m_position(position), m_target(target), m_up(up)
{
}

void Camera::SetAspectFromFramebuffer(int width, int height)
{
    if (width > 0 && height > 0)
    {
        m_aspect = static_cast<float>(width) / static_cast<float>(height);
    }
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(m_position, m_target, m_up);
}

glm::mat4 Camera::GetProjectionMatrix() const
{
    return glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
}

glm::vec3 Camera::GetPosition() const
{
    return m_position;
}
