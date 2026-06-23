#include "engine/camera/Camera.h"
#include "engine/platform/Input.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

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
    if (!input.IsCapturingMouse())
    {
        return;
    }

    float velocity = m_movementSpeed * deltaTime;
    if (input.IsKeyDown(GLFW_KEY_LEFT_SHIFT) || input.IsKeyDown(GLFW_KEY_RIGHT_SHIFT))
    {
        velocity *= 3.0f;
    }

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

glm::vec3 Camera::GetFront() const
{
    return m_front;
}

float Camera::GetYaw() const
{
    return m_yaw;
}

float Camera::GetPitch() const
{
    return m_pitch;
}

void Camera::SetPosition(const glm::vec3& position)
{
    m_position = position;
}

void Camera::SetOrientation(float yaw, float pitch)
{
    m_yaw = yaw;
    m_pitch = std::clamp(pitch, -89.0f, 89.0f);
    UpdateCameraVectors();
}

void Camera::SetOrientationFromDirection(const glm::vec3& direction)
{
    const glm::vec3 front = glm::normalize(direction);
    m_yaw = glm::degrees(atan2f(front.z, front.x));
    m_pitch = glm::degrees(asinf(std::clamp(front.y, -1.0f, 1.0f)));
    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
    UpdateCameraVectors();
}

void Camera::SetLens(float fovDegrees, float nearPlane, float farPlane)
{
    m_fov = fovDegrees;
    m_near = nearPlane;
    m_far = farPlane;
}

void Camera::SetAspect(float aspect)
{
    if (aspect > 0.0f)
    {
        m_aspect = aspect;
    }
}

glm::mat4 Camera::BuildViewMatrixLookingAt(const glm::vec3& target, float distance) const
{
    const glm::vec3 eye = target - m_front * distance;
    return glm::lookAt(eye, target, m_worldUp);
}

void Camera::ApplyViewManipulateResult(
    const glm::mat4& view,
    const glm::vec3& target,
    float distance)
{
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 front = glm::normalize(-glm::vec3(invView[2]));
    m_yaw = glm::degrees(atan2f(front.z, front.x));
    m_pitch = glm::degrees(asinf(std::clamp(front.y, -1.0f, 1.0f)));
    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
    UpdateCameraVectors();
    m_position = target - m_front * distance;
}

void Camera::FrameTarget(const glm::vec3& target, float boundsRadius)
{
    const float distance = ComputeFitDistance(boundsRadius);
    glm::vec3 approach = m_position - target;
    if (glm::dot(approach, approach) < 1e-4f)
    {
        approach = -m_front;
    }
    else
    {
        approach = glm::normalize(approach);
    }

    m_position = target + approach * distance;
    SetOrientationFromDirection(target - m_position);
}

float Camera::GetNearPlane() const
{
    return m_near;
}

float Camera::GetFarPlane() const
{
    return m_far;
}

float Camera::GetFov() const
{
    return m_fov;
}

float Camera::GetAspect() const
{
    return m_aspect;
}

float Camera::ComputeFitDistance(float boundsRadius, float padding) const
{
    const float halfFovY = glm::radians(m_fov * 0.5f);
    const float halfFovX = atanf(tanf(halfFovY) * m_aspect);
    const float limitingHalfFov = std::min(halfFovX, halfFovY);
    const float sinLimit = sinf(limitingHalfFov);
    const float safeRadius = std::max(boundsRadius, 0.05f);
    const float distance =
        sinLimit > 1e-4f ? (safeRadius * padding) / sinLimit : safeRadius * padding;
    return std::max(distance, 0.5f);
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