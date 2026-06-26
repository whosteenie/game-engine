#pragma once

#include <glm/glm.hpp>

class Input;

class Camera
{
public:
    Camera(const glm::vec3& position, float yaw, float pitch);

    void SetAspectFromFramebuffer(int width, int height);

    void ProcessKeyboard(const Input& input, float deltaTime);
    void ProcessMouseMovement(float xOffset, float yOffset);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;
    glm::vec3 GetPosition() const;
    glm::vec3 GetFront() const;
    float GetYaw() const;
    float GetPitch() const;
    void SetPosition(const glm::vec3& position);
    void SetOrientation(float yaw, float pitch);
    void SetOrientationFromDirection(const glm::vec3& direction);
    void SetLens(float fovDegrees, float nearPlane, float farPlane);
    void SetAspect(float aspect);

    glm::mat4 BuildViewMatrixLookingAt(const glm::vec3& target, float distance) const;
    void ApplyViewManipulateResult(const glm::mat4& view, const glm::vec3& target, float distance);
    void FrameTarget(const glm::vec3& target, float boundsRadius);

    float GetNearPlane() const;
    float GetFarPlane() const;
    float GetFov() const;
    float GetAspect() const;

    void SetProjectionJitter(const glm::vec2& jitterNdc);
    void ClearProjectionJitter();
    glm::vec2 GetProjectionJitter() const;

private:
    void UpdateCameraVectors();
    glm::mat4 BuildViewMatrixFromState() const;
    float ComputeFitDistance(float boundsRadius, float padding = 1.2f) const;

    glm::vec3 m_position;
    glm::vec3 m_front;
    glm::vec3 m_up;
    glm::vec3 m_right;
    glm::vec3 m_worldUp;

    float m_yaw;
    float m_pitch;

    float m_movementSpeed = 4.0f;
    float m_mouseSensitivity = 0.1f;

    float m_fov = 45.0f;
    float m_aspect = 1.0f;
    float m_near = 0.1f;
    float m_far = 10000.0f;

    glm::vec2 m_projectionJitterNdc{0.0f};
};