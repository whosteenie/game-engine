#include <glad/glad.h>

#include "engine/SelectionRenderer.h"

#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Mesh.h"
#include "engine/SceneHierarchy.h"
#include "engine/Shader.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <memory>

namespace
{
    constexpr float kOutlineWidthPixels = 2.0f;
    constexpr float kOutlineWidthWorld = 0.004f;
    constexpr float kRadialExpand = 0.012f;
    constexpr float kGlowBlurRadius = 2.0f;
    constexpr float kGlowIntensity = 1.1f;
    constexpr glm::vec3 kSelectionColor(1.0f, 0.85f, 0.22f);
    constexpr glm::vec3 kSelectionGlowColor(1.15f, 0.95f, 0.35f);

    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };

    void DrawMeshesMask(
        const Shader& shader,
        const Camera& camera,
        const std::vector<SelectionMeshDraw>& meshes)
    {
        shader.SetMat4("uView", camera.GetViewMatrix());
        shader.SetMat4("uProjection", camera.GetProjectionMatrix());

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

    void DrawMeshesHull(
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
    : m_maskShader(std::make_unique<Shader>(
          EngineConstants::SelectionMaskVertexShader,
          EngineConstants::SelectionMaskFragmentShader)),
      m_edgeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionEdgeFragmentShader)),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomBlurFragmentShader)),
      m_glowShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionGlowFragmentShader)),
      m_sharpShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionSharpFragmentShader)),
      m_hullShader(std::make_unique<Shader>(
          EngineConstants::SelectionOutlineVertexShader,
          EngineConstants::SelectionOutlineFragmentShader))
{
    CreateFullscreenQuad();
}

SelectionRenderer::~SelectionRenderer()
{
    DestroyTargets();

    if (m_quadVbo != 0)
    {
        glDeleteBuffers(1, &m_quadVbo);
    }

    if (m_quadVao != 0)
    {
        glDeleteVertexArrays(1, &m_quadVao);
    }
}

void SelectionRenderer::CreateFullscreenQuad()
{
    glGenVertexArrays(1, &m_quadVao);
    glGenBuffers(1, &m_quadVbo);
    glBindVertexArray(m_quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(float),
        reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

void SelectionRenderer::CreateColorTarget(unsigned int& fbo, unsigned int& texture, int width, int height) const
{
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &texture);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    const unsigned int attachments[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, attachments);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SelectionRenderer::DestroyColorTarget(unsigned int& fbo, unsigned int& texture) const
{
    if (texture != 0)
    {
        glDeleteTextures(1, &texture);
        texture = 0;
    }

    if (fbo != 0)
    {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
}

void SelectionRenderer::DestroyTargets() const
{
    DestroyColorTarget(m_maskFbo, m_maskTexture);
    DestroyColorTarget(m_edgeFbo, m_edgeTexture);
    DestroyColorTarget(m_glowBlurFbo, m_glowBlurTexture);
    DestroyColorTarget(m_glowBlur2Fbo, m_glowBlur2Texture);
    m_targetWidth = 0;
    m_targetHeight = 0;
}

void SelectionRenderer::ResizeTargets(int width, int height) const
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (m_targetWidth == width && m_targetHeight == height && m_maskFbo != 0)
    {
        return;
    }

    DestroyTargets();

    CreateColorTarget(m_maskFbo, m_maskTexture, width, height);
    CreateColorTarget(m_edgeFbo, m_edgeTexture, width, height);

    const int blurWidth = std::max(1, width / 2);
    const int blurHeight = std::max(1, height / 2);
    CreateColorTarget(m_glowBlurFbo, m_glowBlurTexture, blurWidth, blurHeight);
    CreateColorTarget(m_glowBlur2Fbo, m_glowBlur2Texture, blurWidth, blurHeight);

    m_targetWidth = width;
    m_targetHeight = height;
}

void SelectionRenderer::DrawFullscreenQuad() const
{
    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void SelectionRenderer::DrawScreenSpace(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    int width,
    int height) const
{
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
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
    GLboolean scissorTestEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint blendSrcRgb = GL_SRC_ALPHA;
    GLint blendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);

    ResizeTargets(width, height);

    const int blurWidth = std::max(1, width / 2);
    const int blurHeight = std::max(1, height / 2);
    const float blurTexelSizeX = 1.0f / static_cast<float>(blurWidth);
    const float blurTexelSizeY = 1.0f / static_cast<float>(blurHeight);

    glDisable(GL_MULTISAMPLE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // 1) Object mask (full silhouette, ignores scene depth).
    glViewport(0, 0, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, m_maskFbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_maskShader->Use();
    DrawMeshesMask(*m_maskShader, camera, meshes);

    // 2) Edge extraction to offscreen buffer.
    glBindFramebuffer(GL_FRAMEBUFFER, m_edgeFbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_edgeShader->Use();
    m_edgeShader->SetInt("uMask", 0);
    m_edgeShader->SetVec2(
        "uTexelSize",
        glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)));
    m_edgeShader->SetFloat("uOutlineWidth", kOutlineWidthPixels);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_maskTexture);
    DrawFullscreenQuad();

    // 3) Blur edge mask for subtle glow (fixed internal settings).
    glBindFramebuffer(GL_FRAMEBUFFER, m_glowBlurFbo);
    glViewport(0, 0, blurWidth, blurHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_blurShader->Use();
    m_blurShader->SetInt("uInput", 0);
    m_blurShader->SetFloat("uDirectionX", blurTexelSizeX);
    m_blurShader->SetFloat("uDirectionY", 0.0f);
    m_blurShader->SetFloat("uBlurRadius", kGlowBlurRadius);
    glBindTexture(GL_TEXTURE_2D, m_edgeTexture);
    DrawFullscreenQuad();

    glBindFramebuffer(GL_FRAMEBUFFER, m_glowBlur2Fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_blurShader->Use();
    m_blurShader->SetInt("uInput", 0);
    m_blurShader->SetFloat("uDirectionX", 0.0f);
    m_blurShader->SetFloat("uDirectionY", blurTexelSizeY);
    m_blurShader->SetFloat("uBlurRadius", kGlowBlurRadius);
    glBindTexture(GL_TEXTURE_2D, m_glowBlurTexture);
    DrawFullscreenQuad();

    // 4) Additive glow, then crisp outline on top.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);

    glBlendFunc(GL_ONE, GL_ONE);
    m_glowShader->Use();
    m_glowShader->SetInt("uGlow", 0);
    m_glowShader->SetInt("uEdge", 1);
    m_glowShader->SetVec3("uColor", kSelectionGlowColor);
    m_glowShader->SetFloat("uGlowIntensity", kGlowIntensity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_glowBlur2Texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_edgeTexture);
    DrawFullscreenQuad();

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_sharpShader->Use();
    m_sharpShader->SetInt("uEdge", 0);
    m_sharpShader->SetVec3("uColor", kSelectionColor);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_edgeTexture);
    DrawFullscreenQuad();

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!blendEnabled)
    {
        glDisable(GL_BLEND);
    }
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);

    if (!stencilTestEnabled)
    {
        glDisable(GL_STENCIL_TEST);
    }
    else
    {
        glEnable(GL_STENCIL_TEST);
    }

    if (scissorTestEnabled)
    {
        glEnable(GL_SCISSOR_TEST);
    }

    glCullFace(cullFaceMode);
    if (cullFaceEnabled)
    {
        glEnable(GL_CULL_FACE);
    }
    else
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

void SelectionRenderer::DrawHullFallback(
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

    m_hullShader->Use();
    m_hullShader->SetMat4("uView", camera.GetViewMatrix());
    m_hullShader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_hullShader->SetVec3("uColor", kSelectionColor);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    DrawMeshesHull(*m_hullShader, meshes, 0.0f, viewportHeight);

    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glStencilMask(0x00);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    glCullFace(GL_FRONT);
    DrawMeshesHull(*m_hullShader, meshes, kOutlineWidthPixels, viewportHeight);

    glCullFace(GL_BACK);
    DrawMeshesHull(*m_hullShader, meshes, kOutlineWidthPixels, viewportHeight);

    glStencilMask(stencilWriteMask);
    glStencilFunc(stencilFunc, stencilRef, stencilMask);
    glStencilOp(stencilFail, stencilZFail, stencilZPass);
    if (!stencilTestEnabled)
    {
        glDisable(GL_STENCIL_TEST);
    }

    glCullFace(cullFaceMode);
    if (cullFaceEnabled)
    {
        glEnable(GL_CULL_FACE);
    }
    else
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

void SelectionRenderer::Draw(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    bool useScreenSpace) const
{
    if (meshes.empty())
    {
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);

    if (useScreenSpace)
    {
        DrawScreenSpace(camera, meshes, viewport[2], viewport[3]);
    }
    else
    {
        DrawHullFallback(camera, meshes, viewport[2], viewport[3]);
    }
}
