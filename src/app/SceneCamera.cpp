#include "app/SceneCamera.h"

#include "app/Scene.h"
#include "engine/Camera.h"
#include "engine/CameraComponent.h"
#include "engine/SceneObject.h"

#include <glm/gtc/matrix_transform.hpp>

#include <limits>

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

    int FindActiveCameraObjectIndex(const Scene& scene)
    {
        int bestIndex = -1;
        int bestDepth = std::numeric_limits<int>::max();

        const std::vector<SceneObject>& objects = scene.GetObjects();
        for (int index = 0; index < static_cast<int>(objects.size()); ++index)
        {
            const SceneObject& object = objects[static_cast<std::size_t>(index)];
            if (!object.HasCamera())
            {
                continue;
            }

            const CameraComponent& camera = object.GetCamera();
            if (!camera.enabled)
            {
                continue;
            }

            if (camera.isMain)
            {
                return index;
            }

            if (camera.depth < bestDepth)
            {
                bestDepth = camera.depth;
                bestIndex = index;
            }
        }

        return bestIndex;
    }
}

bool SceneCamera::SceneHasActiveCamera(const Scene& scene)
{
    return FindActiveCameraObjectIndex(scene) >= 0;
}

std::optional<SceneCamera> SceneCamera::TryFromScene(const Scene& scene, float aspect)
{
    const int objectIndex = FindActiveCameraObjectIndex(scene);
    if (objectIndex < 0)
    {
        return std::nullopt;
    }

    const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
    const CameraComponent& component = object.GetCamera();
    const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);
    const glm::mat3 rotationMatrix = glm::mat3(worldMatrix);

    SceneCamera sceneCamera;
    sceneCamera.m_position = glm::vec3(worldMatrix[3]);
    sceneCamera.m_forward =
        NormalizeOrFallback(-glm::vec3(rotationMatrix[2]), glm::vec3(0.0f, 0.0f, -1.0f));
    sceneCamera.m_up = NormalizeOrFallback(glm::vec3(rotationMatrix[1]), glm::vec3(0.0f, 1.0f, 0.0f));
    sceneCamera.m_fovDegrees = component.fovDegrees;
    sceneCamera.m_nearPlane = component.nearPlane;
    sceneCamera.m_farPlane = component.farPlane;
    sceneCamera.m_aspect = aspect;
    return sceneCamera;
}

glm::mat4 SceneCamera::GetViewMatrix() const
{
    return glm::lookAt(m_position, m_position + m_forward, m_up);
}

glm::mat4 SceneCamera::GetProjectionMatrix() const
{
    return glm::perspective(glm::radians(m_fovDegrees), m_aspect, m_nearPlane, m_farPlane);
}

glm::vec3 SceneCamera::GetPosition() const
{
    return m_position;
}

Camera SceneCamera::ToRenderCamera() const
{
    Camera camera(m_position, 0.0f, 0.0f);
    camera.SetOrientationFromDirection(m_forward);
    camera.SetLens(m_fovDegrees, m_nearPlane, m_farPlane);
    camera.SetAspect(m_aspect);
    return camera;
}
