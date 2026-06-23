#pragma once

#include <glm/glm.hpp>

#include <optional>

class Camera;
class Scene;

class SceneCamera
{
public:
    static bool SceneHasActiveCamera(const Scene& scene);
    static std::optional<SceneCamera> TryFromScene(const Scene& scene, float aspect);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;
    glm::vec3 GetPosition() const;

    Camera ToRenderCamera() const;

private:
    glm::vec3 m_position = glm::vec3(0.0f);
    glm::vec3 m_forward = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 m_up = glm::vec3(0.0f, 1.0f, 0.0f);
    float m_fovDegrees = 45.0f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 100.0f;
    float m_aspect = 1.0f;
};
