#include <glad/glad.h>

#include "engine/SelectionRenderer.h"

#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Mesh.h"
#include "engine/SceneHierarchy.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <memory>

namespace
{
    constexpr float kOutlineWidthPixels = 2.5f;
    constexpr float kOutlineWidthWorld = 0.004f;
    constexpr glm::vec3 kSelectionColor(1.0f, 0.82f, 0.2f);

    void DrawMeshes(
        const Shader& shader,
        const std::vector<SelectionMeshDraw>& meshes,
        float outlineWidthPixels,
        float outlineWidthWorld,
        float viewportHeight)
    {
        shader.SetFloat("uOutlineWidth", outlineWidthPixels);
        shader.SetFloat("uOutlineWidthWorld", outlineWidthWorld);
        shader.SetFloat("uViewportHeight", viewportHeight);

        for (const SelectionMeshDraw& meshDraw : meshes)
        {
            if (meshDraw.mesh == nullptr)
            {
                continue;
            }

            shader.SetMat4("uModel", meshDraw.worldMatrix);
            meshDraw.mesh->Draw();
        }
    }
}

SelectionRenderer::SelectionRenderer()
    : m_shader(std::make_unique<Shader>(
          EngineConstants::SelectionOutlineVertexShader,
          EngineConstants::SelectionOutlineFragmentShader))
{
}

SelectionRenderer::~SelectionRenderer() = default;

void SelectionRenderer::Draw(const Camera& camera, const std::vector<SelectionMeshDraw>& meshes) const
{
    if (meshes.empty())
    {
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const float viewportHeight = static_cast<float>(viewport[3]);

    GLboolean multisampleEnabled = glIsEnabled(GL_MULTISAMPLE);
    GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLint depthFunc = GL_LEQUAL;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    GLint cullFaceMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &cullFaceMode);
    GLboolean polygonOffsetFillEnabled = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
    GLint stencilFunc = GL_ALWAYS;
    GLint stencilRef = 0;
    GLint stencilMask = 0xFF;
    GLint stencilFail = GL_KEEP;
    GLint stencilZFail = GL_KEEP;
    GLint stencilZPass = GL_KEEP;
    glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &stencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilMask);
    glGetIntegerv(GL_STENCIL_FAIL, &stencilFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilZFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilZPass);
    GLint stencilWriteMask = 0xFF;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask);
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

    glDisable(GL_MULTISAMPLE);

    if (!depthTestEnabled)
    {
        glEnable(GL_DEPTH_TEST);
    }

    glDepthFunc(GL_LEQUAL);
    glClear(GL_STENCIL_BUFFER_BIT);

    m_shader->Use();
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uColor", kSelectionColor);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -2.0f);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    DrawMeshes(*m_shader, meshes, 0.0f, 0.0f, viewportHeight);

    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glPolygonOffset(0.0f, 0.0f);
    glStencilMask(0x00);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    DrawMeshes(*m_shader, meshes, kOutlineWidthPixels, kOutlineWidthWorld, viewportHeight);

    if (!polygonOffsetFillEnabled)
    {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    glStencilMask(stencilWriteMask);
    glStencilFunc(stencilFunc, stencilRef, stencilMask);
    glStencilOp(stencilFail, stencilZFail, stencilZPass);
    if (!stencilTestEnabled)
    {
        glDisable(GL_STENCIL_TEST);
    }

    glCullFace(cullFaceMode);
    if (!cullFaceEnabled)
    {
        glDisable(GL_CULL_FACE);
    }

    glDepthMask(depthMask);
    glDepthFunc(depthFunc);
    if (!depthTestEnabled)
    {
        glDisable(GL_DEPTH_TEST);
    }

    if (multisampleEnabled)
    {
        glEnable(GL_MULTISAMPLE);
    }
}
