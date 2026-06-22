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
    constexpr float kOutlineWidthPixels = 2.0f;
    constexpr float kOutlineWidthWorld = 0.004f;
    constexpr float kRadialExpand = 0.012f;
    constexpr glm::vec3 kSelectionColor(1.0f, 0.82f, 0.2f);

    void DrawMeshes(
        const Shader& shader,
        const std::vector<SelectionMeshDraw>& meshes,
        float outlineWidthPixels,
        float viewportHeight)
    {
        const bool outlinePass = outlineWidthPixels > 0.0f;
        shader.SetFloat("uOutlineWidth", outlineWidthPixels);
        shader.SetFloat("uOutlineWidthWorld", outlinePass ? kOutlineWidthWorld : 0.0f);
        shader.SetFloat("uRadialExpand", outlinePass ? kRadialExpand : 0.0f);
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
    : m_outlineShader(std::make_unique<Shader>(
          EngineConstants::SelectionOutlineVertexShader,
          EngineConstants::SelectionOutlineFragmentShader))
{
}

SelectionRenderer::~SelectionRenderer() = default;

void SelectionRenderer::DrawOutline(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    int width,
    int height) const
{
    const float viewportHeight = static_cast<float>(height);

    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, previousViewport);

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
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, width, height);
    glClear(GL_STENCIL_BUFFER_BIT);

    m_outlineShader->Use();
    m_outlineShader->SetMat4("uView", camera.GetViewMatrix());
    m_outlineShader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_outlineShader->SetVec3("uColor", kSelectionColor);

    // Pass 1: mark the selected mesh footprint in stencil (ignores scene depth).
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    DrawMeshes(*m_outlineShader, meshes, 0.0f, viewportHeight);

    // Pass 2+3: draw the extruded shell on top of the scene (x-ray outline).
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glStencilMask(0x00);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    glCullFace(GL_FRONT);
    DrawMeshes(*m_outlineShader, meshes, kOutlineWidthPixels, viewportHeight);

    glCullFace(GL_BACK);
    DrawMeshes(*m_outlineShader, meshes, kOutlineWidthPixels, viewportHeight);

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
    if (depthTestEnabled)
    {
        glEnable(GL_DEPTH_TEST);
    }
    else
    {
        glDisable(GL_DEPTH_TEST);
    }

    if (multisampleEnabled)
    {
        glEnable(GL_MULTISAMPLE);
    }

    glViewport(
        previousViewport[0],
        previousViewport[1],
        previousViewport[2],
        previousViewport[3]);
}

void SelectionRenderer::Draw(const Camera& camera, const std::vector<SelectionMeshDraw>& meshes) const
{
    if (meshes.empty())
    {
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    DrawOutline(camera, meshes, viewport[2], viewport[3]);
}
