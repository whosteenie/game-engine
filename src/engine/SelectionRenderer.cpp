#include <glad/glad.h>

#include "engine/SelectionRenderer.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/GizmoGeometry.h"
#include "engine/SceneObject.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

SelectionRenderer::SelectionRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::GridFragmentShader))
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

SelectionRenderer::~SelectionRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
}

void SelectionRenderer::Draw(
    const Camera& camera,
    const SceneObject& object,
    const glm::mat4& worldMatrix) const
{
    std::vector<float> vertices;

    if (object.IsRenderable())
    {
        GizmoGeometry::AppendOrientedBoxOutline(
            vertices,
            worldMatrix,
            object.GetLocalBoundsMin(),
            object.GetLocalBoundsMax(),
            0.03f);
    }
    else
    {
        const glm::vec3 pivotBoundsMin(-0.15f);
        const glm::vec3 pivotBoundsMax(0.15f);
        GizmoGeometry::AppendOrientedBoxOutline(
            vertices,
            worldMatrix,
            pivotBoundsMin,
            pivotBoundsMax,
            0.02f);
    }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);

    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uColor", glm::vec3(1.0f, 0.82f, 0.2f));
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size() / 3));
    glBindVertexArray(0);
}
