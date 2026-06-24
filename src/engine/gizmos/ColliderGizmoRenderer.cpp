#include "engine/gizmos/ColliderGizmoRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/components/ColliderComponent.h"
#include "engine/gizmos/GizmoDraw.h"
#include "engine/gizmos/GizmoGeometry.h"
#include "engine/rendering/Constants.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/Transform.h"
#include "engine/rendering/Shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <vector>

namespace
{
    constexpr int kCircleSegments = 32;
    constexpr glm::vec3 kColliderGizmoColor(0.28f, 0.92f, 0.38f);

    glm::vec3 GizmoColor(bool selected)
    {
        if (selected)
        {
            return glm::min(kColliderGizmoColor * 1.25f, glm::vec3(1.0f));
        }

        return kColliderGizmoColor * 0.85f;
    }

    void AppendCircle(
        std::vector<float>& vertices,
        const glm::vec3& center,
        const glm::vec3& axis0,
        const glm::vec3& axis1,
        float radius,
        int segments)
    {
        glm::vec3 previousPoint = center + axis0 * radius;
        for (int segment = 1; segment <= segments; ++segment)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
            const glm::vec3 point =
                center + (axis0 * std::cos(angle) + axis1 * std::sin(angle)) * radius;
            GizmoGeometry::AppendLine(vertices, previousPoint, point);
            previousPoint = point;
        }
    }

    void AppendSphereWireframe(std::vector<float>& vertices, const glm::vec3& center, float radius)
    {
        AppendCircle(vertices, center, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), radius, kCircleSegments);
        AppendCircle(vertices, center, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), radius, kCircleSegments);
        AppendCircle(vertices, center, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), radius, kCircleSegments);
    }

    void AppendOrientedBoxWireframe(
        std::vector<float>& vertices,
        const glm::vec3& center,
        const glm::quat& rotation,
        const glm::vec3& halfExtents)
    {
        const glm::mat3 rotationMatrix = glm::mat3_cast(rotation);
        const float signs[2] = {-1.0f, 1.0f};
        glm::vec3 corners[8];

        int cornerIndex = 0;
        for (float sx : signs)
        {
            for (float sy : signs)
            {
                for (float sz : signs)
                {
                    const glm::vec3 localCorner = glm::vec3(sx, sy, sz) * halfExtents;
                    corners[cornerIndex++] = center + rotationMatrix * localCorner;
                }
            }
        }

        constexpr int edges[12][2] = {
            {0, 1}, {1, 3}, {3, 2}, {2, 0},
            {4, 5}, {5, 7}, {7, 6}, {6, 4},
            {0, 4}, {1, 5}, {2, 6}, {3, 7},
        };

        for (const auto& edge : edges)
        {
            GizmoGeometry::AppendLine(vertices, corners[edge[0]], corners[edge[1]]);
        }
    }

    void AppendColliderGizmo(
        std::vector<float>& vertices,
        const ColliderComponent& collider,
        const glm::mat4& worldMatrix)
    {
        const Transform worldTransform = Transform::FromMatrix(worldMatrix);
        const glm::vec3 colliderCenter = glm::vec3(worldMatrix * glm::vec4(collider.offset, 1.0f));

        const glm::vec3 absScale = glm::abs(worldTransform.scale);
        if (collider.shape == ColliderShape::Sphere)
        {
            const float radius =
                collider.radius * std::max(absScale.x, std::max(absScale.y, absScale.z));
            AppendSphereWireframe(vertices, colliderCenter, std::max(radius, 0.01f));
            return;
        }

        const glm::vec3 scaledHalfExtents = glm::max(collider.halfExtents * absScale, glm::vec3(0.01f));
        AppendOrientedBoxWireframe(vertices, colliderCenter, worldTransform.rotation, scaledHalfExtents);
    }
}

ColliderGizmoRenderer::ColliderGizmoRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::LineFragmentShader))
{
}

ColliderGizmoRenderer::~ColliderGizmoRenderer() = default;

void ColliderGizmoRenderer::Draw(
    const Camera& camera,
    const std::vector<SceneObject>& objects,
    const std::function<glm::mat4(int objectIndex)>& getWorldMatrix,
    const std::vector<int>& selectedObjectIndices) const
{
    if (objects.empty())
    {
        return;
    }

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasCollider())
        {
            continue;
        }

        const bool selected = std::find(
                                  selectedObjectIndices.begin(),
                                  selectedObjectIndices.end(),
                                  objectIndex)
            != selectedObjectIndices.end();
        if (!selected)
        {
            continue;
        }

        std::vector<float> vertices;
        AppendColliderGizmo(vertices, object.GetCollider(), getWorldMatrix(objectIndex));
        if (vertices.empty())
        {
            continue;
        }

        GizmoDraw::DrawLineVertices(*m_shader, camera, vertices, GizmoColor(true));
    }
}
