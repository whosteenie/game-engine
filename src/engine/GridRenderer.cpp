#include <glad/glad.h>

#include "engine/GridRenderer.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <vector>
#include <memory>

GridRenderer::GridRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::GridFragmentShader))
{
    BuildGridGeometry(m_halfExtent);
}

GridRenderer::~GridRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteBuffers(1, &m_ebo);
}

void GridRenderer::BuildGridGeometry(float halfExtent)
{
    const float y = 0.051f;

    const std::vector<float> vertices = {
        -halfExtent, y, -halfExtent,
         halfExtent, y, -halfExtent,
         halfExtent, y,  halfExtent,
        -halfExtent, y,  halfExtent,
    };

    const std::vector<unsigned int> indices = {
        0, 1, 2,
        0, 2, 3,
    };

    m_indexCount = static_cast<unsigned int>(indices.size());

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void GridRenderer::Draw(const Camera& camera, bool outputLinear) const
{
    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uColor", glm::vec3(0.35f, 0.38f, 0.42f));
    m_shader->SetVec3("uCameraPosition", camera.GetPosition());
    m_shader->SetFloat("uCellSize", m_cellSize);
    m_shader->SetFloat("uMajorInterval", m_majorInterval);
    m_shader->SetInt("uOutputLinear", outputLinear ? 1 : 0);

    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean depthMaskEnabled = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskEnabled);
    GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    GLint blendSrcRgb;
    GLint blendDstRgb;
    GLint blendSrcAlpha;
    GLint blendDstAlpha;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glDepthMask(depthMaskEnabled);

    if (cullFaceEnabled)
    {
        glEnable(GL_CULL_FACE);
    }
    else
    {
        glDisable(GL_CULL_FACE);
    }

    if (blendEnabled)
    {
        glEnable(GL_BLEND);
    }
    else
    {
        glDisable(GL_BLEND);
    }

    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
}
