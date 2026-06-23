#include <glad/glad.h>

#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/camera/Camera.h"
#include "engine/components/CameraComponent.h"
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

    void AppendLine(std::vector<float>& vertices, const glm::vec3& a, const glm::vec3& b)
    {
        vertices.push_back(a.x);
        vertices.push_back(a.y);
        vertices.push_back(a.z);
        vertices.push_back(b.x);
        vertices.push_back(b.y);
        vertices.push_back(b.z);
    }

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
        AppendLine(vertices, corners[0], corners[1]);
        AppendLine(vertices, corners[1], corners[2]);
        AppendLine(vertices, corners[2], corners[3]);
        AppendLine(vertices, corners[3], corners[0]);
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
            nearCenter + up * nearHalfHeight - right * nearHalfWidth,  // top-left
            nearCenter + up * nearHalfHeight + right * nearHalfWidth,  // top-right
            nearCenter - up * nearHalfHeight + right * nearHalfWidth,  // bottom-right
            nearCenter - up * nearHalfHeight - right * nearHalfWidth   // bottom-left
        };

        glm::vec3 farCorners[4] = {
            farCenter + up * farHalfHeight - right * farHalfWidth,
            farCenter + up * farHalfHeight + right * farHalfWidth,
            farCenter - up * farHalfHeight + right * farHalfWidth,
            farCenter - up * farHalfHeight - right * farHalfWidth
        };

        AppendPlaneRect(vertices, nearCorners);
        AppendPlaneRect(vertices, farCorners);

        for (int i = 0; i < 4; ++i)
        {
            AppendLine(vertices, nearCorners[i], farCorners[i]);
            AppendLine(vertices, position, nearCorners[i]);
        }

        // Forward axis cue so it's clear which way the camera is facing.
        AppendLine(vertices, position, nearCenter);
    }
}

CameraGizmoRenderer::CameraGizmoRenderer()
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

CameraGizmoRenderer::~CameraGizmoRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
}

void CameraGizmoRenderer::Draw(
    const Camera& camera,
    const std::vector<SceneObject>& objects,
    const std::function<glm::mat4(int objectIndex)>& getWorldMatrix,
    const std::vector<int>& selectedObjectIndices) const
{
    if (objects.empty() || selectedObjectIndices.empty())
    {
        return;
    }

    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());

    glBindVertexArray(m_vao);

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

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);
        m_shader->SetVec3("uColor", GizmoColor(true));
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size() / 3));
    }

    glBindVertexArray(0);
}
