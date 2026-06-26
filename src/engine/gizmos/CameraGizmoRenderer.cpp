#include "engine/gizmos/CameraGizmoRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/components/CameraComponent.h"
#include "engine/gizmos/GizmoDraw.h"
#include "engine/gizmos/GizmoGeometry.h"
#include "engine/rendering/Constants.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/Transform.h"
#include "engine/rendering/Shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr float kDefaultAspect = 16.0f / 9.0f;
    constexpr glm::vec3 kCameraGizmoColor(0.38f, 0.72f, 1.0f);

    glm::vec3 GizmoColor(bool selected)
    {
        if (selected)
        {
            return glm::min(kCameraGizmoColor * 1.2f, glm::vec3(1.0f));
        }

        return kCameraGizmoColor;
    }

    void AppendPlaneRect(std::vector<float>& vertices, const glm::vec3 corners[4])
    {
        GizmoGeometry::AppendLine(vertices, corners[0], corners[1]);
        GizmoGeometry::AppendLine(vertices, corners[1], corners[2]);
        GizmoGeometry::AppendLine(vertices, corners[2], corners[3]);
        GizmoGeometry::AppendLine(vertices, corners[3], corners[0]);
    }

    void AppendFrustumGizmo(
        std::vector<float>& vertices,
        const glm::vec3& position,
        const glm::vec3& right,
        const glm::vec3& up,
        const glm::vec3& forward,
        const CameraComponent& cameraComponent)
    {
        const float fovRadians = glm::radians(glm::clamp(cameraComponent.fovDegrees, 1.0f, 179.0f));
        const float nearDistance = std::max(cameraComponent.nearPlane, 0.01f);
        const float farDistance = std::max(cameraComponent.farPlane, nearDistance + 0.01f);
        const float tanHalfFov = std::tan(fovRadians * 0.5f);

        const float nearHalfHeight = tanHalfFov * nearDistance;
        const float nearHalfWidth = nearHalfHeight * kDefaultAspect;
        const float farHalfHeight = tanHalfFov * farDistance;
        const float farHalfWidth = farHalfHeight * kDefaultAspect;

        const glm::vec3 nearCenter = position + forward * nearDistance;
        const glm::vec3 farCenter = position + forward * farDistance;

        glm::vec3 nearCorners[4] = {
            nearCenter + up * nearHalfHeight - right * nearHalfWidth,
            nearCenter + up * nearHalfHeight + right * nearHalfWidth,
            nearCenter - up * nearHalfHeight + right * nearHalfWidth,
            nearCenter - up * nearHalfHeight - right * nearHalfWidth,
        };

        glm::vec3 farCorners[4] = {
            farCenter + up * farHalfHeight - right * farHalfWidth,
            farCenter + up * farHalfHeight + right * farHalfWidth,
            farCenter - up * farHalfHeight + right * farHalfWidth,
            farCenter - up * farHalfHeight - right * farHalfWidth,
        };

        AppendPlaneRect(vertices, nearCorners);
        AppendPlaneRect(vertices, farCorners);

        for (int i = 0; i < 4; ++i)
        {
            GizmoGeometry::AppendLine(vertices, nearCorners[i], farCorners[i]);
            GizmoGeometry::AppendLine(vertices, position, nearCorners[i]);
        }

        GizmoGeometry::AppendLine(vertices, position, nearCenter);
    }
}

CameraGizmoRenderer::CameraGizmoRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GizmoLineVertexShader, EngineConstants::LineFragmentShader))
{
}

CameraGizmoRenderer::~CameraGizmoRenderer() = default;

void CameraGizmoRenderer::Draw(
    const Camera& camera,
    const std::vector<SceneObject>& objects,
    const std::function<glm::mat4(int objectIndex)>& getWorldMatrix,
    const std::vector<int>& selectedObjectIndices,
    const bool depthReadOnly) const
{
    if (objects.empty() || selectedObjectIndices.empty())
    {
        return;
    }

    for (int objectIndex : selectedObjectIndices)
    {
        if (objectIndex < 0 || objectIndex >= static_cast<int>(objects.size()))
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasCamera())
        {
            continue;
        }

        const Transform worldTransform = Transform::FromMatrix(getWorldMatrix(objectIndex));
        const glm::vec3& worldPosition = worldTransform.position;
        const glm::quat& worldRotation = worldTransform.rotation;

        const glm::vec3 right = glm::normalize(worldRotation * glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 up = glm::normalize(worldRotation * glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 forward = glm::normalize(worldRotation * glm::vec3(0.0f, 0.0f, -1.0f));

        std::vector<float> vertices;
        AppendFrustumGizmo(vertices, worldPosition, right, up, forward, object.GetCamera());
        if (vertices.empty())
        {
            continue;
        }

        GizmoDraw::DrawLineVertices(*m_shader, camera, vertices, GizmoColor(true), depthReadOnly);
    }
}
