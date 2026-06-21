#include <glad/glad.h>

#include "engine/GridRenderer.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <vector>

GridRenderer::GridRenderer()
    : m_shader(new Shader(EngineConstants::GridVertexShader, EngineConstants::GridFragmentShader))
{
    BuildGridGeometry(10, 1.0f);
}

GridRenderer::~GridRenderer()
{
    delete m_shader;
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
}

void GridRenderer::BuildGridGeometry(int halfExtent, float spacing)
{
    std::vector<float> vertices;
    const float extent = halfExtent * spacing;
    const float y = 0.002f; // draw above the shadow floor so lines stay visible

    for (int i = -halfExtent; i <= halfExtent; ++i)
    {
        float pos = i * spacing;

        // Line along X (varying Z)
        vertices.insert(vertices.end(), { -extent, y, pos,  extent, y, pos });
        // Line along Z (varying X)
        vertices.insert(vertices.end(), { pos, y, -extent,  pos, y, extent });
    }

    m_vertexCount = static_cast<unsigned int>(vertices.size() / 3);

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void GridRenderer::Draw(const Camera& camera) const
{
    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uColor", glm::vec3(0.35f, 0.38f, 0.42f));

    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, m_vertexCount);
}