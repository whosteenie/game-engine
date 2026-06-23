#include <glad/glad.h>

#include "engine/LightGizmoRenderer.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Light.h"
#include "engine/LightComponent.h"
#include "engine/SceneObject.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <vector>

namespace
{
    constexpr int CircleSegments = 32;
    constexpr int SphereSegments = 24;
    void AppendLine(std::vector<float>& vertices, const glm::vec3& a, const glm::vec3& b)
    {
        vertices.push_back(a.x);
        vertices.push_back(a.y);
        vertices.push_back(a.z);
        vertices.push_back(b.x);
        vertices.push_back(b.y);
        vertices.push_back(b.z);
    }

    glm::vec3 BuildPerpendicular(const glm::vec3& normal)
    {
        const glm::vec3 reference = glm::abs(normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
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
            AppendLine(vertices, previousPoint, point);
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
            AppendLine(vertices, anchor + offset, anchor + offset + shineDirection * rayLength);
        }

        AppendLine(vertices, anchor, anchor + shineDirection * (rayLength * 0.55f));
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
            const glm::vec3 rimPoint = baseCenter + (tangent * std::cos(angle) + bitangent * std::sin(angle)) * baseRadius;
            AppendLine(vertices, position, rimPoint);
        }

        AppendLine(vertices, position, baseCenter);
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
            const float outerCutoffDegrees = glm::degrees(std::acos(glm::clamp(light.GetOuterCutoffCos(), -1.0f, 1.0f)));
            AppendSpotGizmo(vertices, light.GetPosition(), light.GetDirection(), outerCutoffDegrees, light.GetRange());
            break;
        }
        }
    }
}

LightGizmoRenderer::LightGizmoRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::LineFragmentShader))
{
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

LightGizmoRenderer::~LightGizmoRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
}

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

    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());

    glBindVertexArray(m_vao);

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
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

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

        const bool selected = std::find(
                                  selectedObjectIndices.begin(),
                                  selectedObjectIndices.end(),
                                  objectIndex)
            != selectedObjectIndices.end();
        m_shader->SetVec3("uColor", GizmoColor(light, selected));
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size() / 3));
    }

    glBindVertexArray(0);
}
