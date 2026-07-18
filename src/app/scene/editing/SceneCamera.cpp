#include "app/scene/editing/SceneCamera.h"

#include "app/scene/document/Scene.h"
#include "engine/camera/Camera.h"
#include "engine/components/CameraComponent.h"
#include "engine/scene/RotationUtils.h"
#include "engine/scene/SceneObject.h"

#include <glm/gtc/matrix_transform.hpp>

#include <limits>

namespace
{
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

    const SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    const CameraComponent& component = object.GetCamera();
    const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);

    SceneCamera sceneCamera;
    RotationUtils::ExtractCameraBasis(
        worldMatrix,
        sceneCamera.m_position,
        sceneCamera.m_forward,
        sceneCamera.m_up);
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
