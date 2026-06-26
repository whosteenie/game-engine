#include "engine/gizmos/LightGizmoRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/components/LightComponent.h"
#include "engine/gizmos/GizmoDraw.h"
#include "engine/gizmos/GizmoGeometry.h"
#include "engine/rendering/Constants.h"
#include "engine/lighting/Light.h"
#include "engine/scene/SceneObject.h"
#include "engine/rendering/Shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <vector>

namespace
{
    constexpr int CircleSegments = 32;
    constexpr int SphereSegments = 24;

    glm::vec3 BuildPerpendicular(const glm::vec3& normal)
    {
        const glm::vec3 reference =
            glm::abs(normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        return glm::normalize(glm::cross(reference, normal));
    }

    void AppendCircle(
        std::vector<float>& vertices,
        const glm::vec3& center,
        const glm::vec3& normal,
        float radius,
        int segments)
    {
        const glm::vec3 tangent = BuildPerpendicular(normal);
        const glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));

        glm::vec3 previousPoint = center + tangent * radius;
        for (int segment = 1; segment <= segments; ++segment)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
            const glm::vec3 point = center + (tangent * std::cos(angle) + bitangent * std::sin(angle)) * radius;
            GizmoGeometry::AppendLine(vertices, previousPoint, point);
            previousPoint = point;
        }
    }

    void AppendDirectionalGizmo(
        std::vector<float>& vertices,
        const glm::vec3& anchor,
        const glm::vec3& directionTowardSource)
    {
        const glm::vec3 towardSource = glm::normalize(directionTowardSource);
        const glm::vec3 shineDirection = -towardSource;
        const float discRadius = 0.28f;
        const float rayLength = 1.25f;

        AppendCircle(vertices, anchor, towardSource, discRadius, CircleSegments);

        const glm::vec3 tangent = BuildPerpendicular(towardSource);
        const glm::vec3 bitangent = glm::normalize(glm::cross(towardSource, tangent));

        for (int ray = 0; ray < 4; ++ray)
        {
            const float angle = glm::half_pi<float>() * static_cast<float>(ray);
            const glm::vec3 offset = (tangent * std::cos(angle) + bitangent * std::sin(angle)) * discRadius * 0.65f;
            GizmoGeometry::AppendLine(vertices, anchor + offset, anchor + offset + shineDirection * rayLength);
        }

        GizmoGeometry::AppendLine(vertices, anchor, anchor + shineDirection * (rayLength * 0.55f));
    }

    void AppendSphere(std::vector<float>& vertices, const glm::vec3& center, float radius, int segments)
    {
        AppendCircle(vertices, center, glm::vec3(0.0f, 1.0f, 0.0f), radius, segments);
        AppendCircle(vertices, center, glm::vec3(1.0f, 0.0f, 0.0f), radius, segments);
        AppendCircle(vertices, center, glm::vec3(0.0f, 0.0f, 1.0f), radius, segments);
    }

    void AppendSpotGizmo(
        std::vector<float>& vertices,
        const glm::vec3& position,
        const glm::vec3& directionTowardSource,
        float outerCutoffDegrees,
        float range)
    {
        const glm::vec3 towardSource = glm::normalize(directionTowardSource);
        const glm::vec3 shineDirection = -towardSource;
        const float coneLength = range > 0.0f ? range : 2.5f;
        const float baseRadius = coneLength * std::tan(glm::radians(outerCutoffDegrees));
        const glm::vec3 baseCenter = position + shineDirection * coneLength;

        AppendCircle(vertices, baseCenter, towardSource, baseRadius, CircleSegments);

        const glm::vec3 tangent = BuildPerpendicular(towardSource);
        const glm::vec3 bitangent = glm::normalize(glm::cross(towardSource, tangent));
        const int spokeCount = 8;
        for (int spoke = 0; spoke < spokeCount; ++spoke)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(spoke) / static_cast<float>(spokeCount);
            const glm::vec3 rimPoint =
                baseCenter + (tangent * std::cos(angle) + bitangent * std::sin(angle)) * baseRadius;
            GizmoGeometry::AppendLine(vertices, position, rimPoint);
        }

        GizmoGeometry::AppendLine(vertices, position, baseCenter);
    }

    glm::vec3 GizmoColor(const Light& light, bool selected)
    {
        glm::vec3 color = glm::clamp(light.GetColor() * light.GetIntensity(), glm::vec3(0.05f), glm::vec3(1.0f));
        if (selected)
        {
            color = glm::min(color * 1.35f, glm::vec3(1.0f));
        }
        else
        {
            color *= 0.55f;
        }

        return color;
    }

    void AppendLightGizmo(std::vector<float>& vertices, const Light& light)
    {
        switch (light.GetType())
        {
        case LightType::Directional:
            AppendDirectionalGizmo(vertices, light.GetPosition(), light.GetDirection());
            break;
        case LightType::Point:
            AppendSphere(vertices, light.GetPosition(), 0.2f, SphereSegments);
            break;
        case LightType::Spot:
        {
            const float outerCutoffDegrees =
                glm::degrees(std::acos(glm::clamp(light.GetOuterCutoffCos(), -1.0f, 1.0f)));
            AppendSpotGizmo(vertices, light.GetPosition(), light.GetDirection(), outerCutoffDegrees, light.GetRange());
            break;
        }
        }
    }
}

LightGizmoRenderer::LightGizmoRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GizmoLineVertexShader, EngineConstants::LineFragmentShader))
{
}

LightGizmoRenderer::~LightGizmoRenderer() = default;

void LightGizmoRenderer::Draw(
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
        if (!object.HasLight())
        {
            continue;
        }

        const Light light = BuildLightFromSceneObject(object, getWorldMatrix(objectIndex));

        std::vector<float> vertices;
        AppendLightGizmo(vertices, light);
        if (vertices.empty())
        {
            continue;
        }

        const bool selected = std::find(
                                  selectedObjectIndices.begin(),
                                  selectedObjectIndices.end(),
                                  objectIndex)
            != selectedObjectIndices.end();
        GizmoDraw::DrawLineVertices(*m_shader, camera, vertices, GizmoColor(light, selected));
    }
}
