#include "engine/Camera.h"
#include "engine/Input.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

Camera::Camera(const glm::vec3& position, float yaw, float pitch)
    : m_position(position),
      m_worldUp(0.0f, 1.0f, 0.0f),
      m_yaw(yaw),
      m_pitch(pitch)
{
    UpdateCameraVectors();
}

void Camera::SetAspectFromFramebuffer(int width, int height)
{
    if (width > 0 && height > 0)
    {
        m_aspect = static_cast<float>(width) / static_cast<float>(height);
    }
}

void Camera::ProcessKeyboard(const Input& input, float deltaTime)
{
    float velocity = m_movementSpeed * deltaTime;

    if (input.IsKeyDown(GLFW_KEY_W))
    {
        m_position += m_front * velocity;
    }
    if (input.IsKeyDown(GLFW_KEY_S))
    {
        m_position -= m_front * velocity;
    }
    if (input.IsKeyDown(GLFW_KEY_A))
    {
        m_position -= m_right * velocity;
    }
    if (input.IsKeyDown(GLFW_KEY_D))
    {
        m_position += m_right * velocity;
    }
    if (input.IsKeyDown(GLFW_KEY_Q))
    {
        m_position -= m_worldUp * velocity;
    }
    if (input.IsKeyDown(GLFW_KEY_E))
    {
        m_position += m_worldUp * velocity;
    }
}

void Camera::ProcessMouseMovement(float xOffset, float yOffset)
{
    xOffset *= m_mouseSensitivity;
    yOffset *= m_mouseSensitivity;

    m_yaw += xOffset;
    m_pitch += yOffset;

    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);

    UpdateCameraVectors();
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::GetProjectionMatrix() const
{
    return glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
}

glm::vec3 Camera::GetPosition() const
{
    return m_position;
}

void Camera::UpdateCameraVectors()
{
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    m_right = glm::normalize(glm::cross(m_front, m_worldUp));
    m_up = glm::normalize(glm::cross(m_right, m_front));
}