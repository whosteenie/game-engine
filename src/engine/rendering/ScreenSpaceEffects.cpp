#include <glad/glad.h>

#include "engine/rendering/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Shader.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace
{
    constexpr float kBackgroundSrgb[3] = {0.08f, 0.09f, 0.15f};

    float SrgbChannelToLinear(float channel)
    {
        return std::pow(channel, 2.2f);
    }

    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };

    bool IsPostProcessDebugMode(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::Ssao ||
               mode == RenderDebugMode::CompositeOcclusion;
    }
}

ScreenSpaceEffects::ScreenSpaceEffects()
    : m_sceneFramebuffer(std::make_unique<Framebuffer>()),
      m_ssaoShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoFragmentShader)),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoBlurFragmentShader)),
      m_compositeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ScreenCompositeFragmentShader)),
      m_bloomExtractShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomExtractFragmentShader)),
      m_bloomBlurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomBlurFragmentShader)),
      m_shadowBlurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ShadowBlurFragmentShader)),
      m_tonemapShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::TonemapFragmentShader)),
      m_debugChannelShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DebugChannelFragmentShader))
{
    CreateFullscreenQuad();
    CreateNoiseTexture();
    CreateKernel();
}

ScreenSpaceEffects::~ScreenSpaceEffects()
{
    DestroySingleChannelTarget(m_ssaoFbo, m_ssaoTexture);
    DestroySingleChannelTarget(m_ssaoBlurFbo, m_ssaoBlurTexture);
    DestroySingleChannelTarget(m_shadowBlurFbo, m_shadowBlurTexture);
    DestroySingleChannelTarget(m_shadowBlur2Fbo, m_shadowBlur2Texture);
    DestroyHdrColorTarget(m_hdrCompositeFbo, m_hdrCompositeTexture);
    DestroyHdrColorTarget(m_bloomExtractFbo, m_bloomExtractTexture);
    DestroyHdrColorTarget(m_bloomBlurFbo, m_bloomBlurTexture);
    DestroyHdrColorTarget(m_bloomBlur2Fbo, m_bloomBlur2Texture);

    if (m_noiseTexture != 0)
    {
        glDeleteTextures(1, &m_noiseTexture);
    }

    if (m_quadVbo != 0)
    {
        glDeleteBuffers(1, &m_quadVbo);
    }

    if (m_quadVao != 0)
    {
        glDeleteVertexArrays(1, &m_quadVao);
    }
}

void ScreenSpaceEffects::CreateFullscreenQuad()
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

void ScreenSpaceEffects::CreateNoiseTexture()
{
    std::mt19937 generator(101);
    std::uniform_real_distribution<float> random(-1.0f, 1.0f);

    std::array<glm::vec3, 16> noiseValues{};
    for (glm::vec3& noiseValue : noiseValues)
    {
        noiseValue = glm::normalize(glm::vec3(random(generator), random(generator), 0.0f));
    }

    glGenTextures(1, &m_noiseTexture);
    glBindTexture(GL_TEXTURE_2D, m_noiseTexture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGB16F,
        4,
        4,
        0,
        GL_RGB,
        GL_FLOAT,
        noiseValues.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ScreenSpaceEffects::CreateKernel()
{
    std::mt19937 generator(202);
    std::uniform_real_distribution<float> random(0.0f, 1.0f);

    m_kernelSamples.clear();
    m_kernelSamples.reserve(KernelSampleCount);

    for (int sampleIndex = 0; sampleIndex < KernelSampleCount; ++sampleIndex)
    {
        glm::vec3 sample(
            random(generator) * 2.0f - 1.0f,
            random(generator) * 2.0f - 1.0f,
            random(generator));
        sample = glm::normalize(sample);

        const float scale = static_cast<float>(sampleIndex) / static_cast<float>(KernelSampleCount);
        sample *= random(generator) * glm::mix(0.1f, 1.0f, scale * scale);
        m_kernelSamples.push_back(sample);
    }
}

void ScreenSpaceEffects::CreateSingleChannelTarget(
    unsigned int& fbo,
    unsigned int& texture,
    int width,
    int height) const
{
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &texture);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height, 0, GL_RED, GL_FLOAT, nullptr);
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

void ScreenSpaceEffects::CreateHdrColorTarget(
    unsigned int& fbo,
    unsigned int& texture,
    int width,
    int height) const
{
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &texture);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
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

void ScreenSpaceEffects::DestroyHdrColorTarget(unsigned int& fbo, unsigned int& texture) const
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

void ScreenSpaceEffects::ResizeHdrColorTarget(int width, int height)
{
    DestroyHdrColorTarget(m_hdrCompositeFbo, m_hdrCompositeTexture);
    CreateHdrColorTarget(m_hdrCompositeFbo, m_hdrCompositeTexture, width, height);
}

void ScreenSpaceEffects::ResizeBloomTargets(int width, int height)
{
    const int bloomWidth = std::max(1, width / 2);
    const int bloomHeight = std::max(1, height / 2);

    DestroyHdrColorTarget(m_bloomExtractFbo, m_bloomExtractTexture);
    DestroyHdrColorTarget(m_bloomBlurFbo, m_bloomBlurTexture);
    DestroyHdrColorTarget(m_bloomBlur2Fbo, m_bloomBlur2Texture);

    CreateHdrColorTarget(m_bloomExtractFbo, m_bloomExtractTexture, bloomWidth, bloomHeight);
    CreateHdrColorTarget(m_bloomBlurFbo, m_bloomBlurTexture, bloomWidth, bloomHeight);
    CreateHdrColorTarget(m_bloomBlur2Fbo, m_bloomBlur2Texture, bloomWidth, bloomHeight);
}

void ScreenSpaceEffects::DestroySingleChannelTarget(unsigned int& fbo, unsigned int& texture) const
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

void ScreenSpaceEffects::ResizeSingleChannelTargets(int width, int height)
{
    DestroySingleChannelTarget(m_ssaoFbo, m_ssaoTexture);
    DestroySingleChannelTarget(m_ssaoBlurFbo, m_ssaoBlurTexture);
    DestroySingleChannelTarget(m_shadowBlurFbo, m_shadowBlurTexture);
    DestroySingleChannelTarget(m_shadowBlur2Fbo, m_shadowBlur2Texture);

    CreateSingleChannelTarget(m_ssaoFbo, m_ssaoTexture, width, height);
    CreateSingleChannelTarget(m_ssaoBlurFbo, m_ssaoBlurTexture, width, height);
    CreateSingleChannelTarget(m_shadowBlurFbo, m_shadowBlurTexture, width, height);
    CreateSingleChannelTarget(m_shadowBlur2Fbo, m_shadowBlur2Texture, width, height);
}

void ScreenSpaceEffects::Resize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (m_width == width && m_height == height && m_sceneFramebuffer->IsValid())
    {
        return;
    }

    m_sceneFramebuffer->Resize(width, height, FramebufferColorMode::SplitDirectIndirect);
    ResizeSingleChannelTargets(width, height);
    ResizeHdrColorTarget(width, height);
    ResizeBloomTargets(width, height);
    m_width = width;
    m_height = height;
}

void ScreenSpaceEffects::BeginScenePass() const
{
    m_sceneFramebuffer->Bind();

    const float directClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float indirectClear[] = {
        SrgbChannelToLinear(kBackgroundSrgb[0]),
        SrgbChannelToLinear(kBackgroundSrgb[1]),
        SrgbChannelToLinear(kBackgroundSrgb[2]),
        1.0f};

    if (m_sceneFramebuffer->HasSplitLighting())
    {
        const float normalClear[] = {0.0f, 0.0f, 1.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 0, directClear);
        glClearBufferfv(GL_COLOR, 1, indirectClear);
        if (m_sceneFramebuffer->HasGeometryNormals())
        {
            glClearBufferfv(GL_COLOR, 2, normalClear);
        }
        if (m_sceneFramebuffer->HasShadowFactor())
        {
            const float shadowClear[] = {1.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 3, shadowClear);
        }
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    else
    {
        glClearColor(indirectClear[0], indirectClear[1], indirectClear[2], indirectClear[3]);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

void ScreenSpaceEffects::EndScenePass() const
{
    m_sceneFramebuffer->Unbind();
}

void ScreenSpaceEffects::DrawFullscreenQuad() const
{
    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void ScreenSpaceEffects::Apply(
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    const DirectionalShadowSettings& shadowSettings) const
{
    if (!m_enabled || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    const bool runSsao = m_ssaoEnabled &&
        !(m_debugMode >= RenderDebugMode::ShadowFactor && m_debugMode <= RenderDebugMode::CascadeIndex);

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 inverseProjectionMatrix = glm::inverse(projectionMatrix);
    const glm::vec2 noiseScale(
        static_cast<float>(m_width) / 4.0f,
        static_cast<float>(m_height) / 4.0f);
    const glm::vec2 texelSize(
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    glDisable(GL_DEPTH_TEST);

    if (runSsao)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_ssaoShader->Use();
        m_ssaoShader->SetInt("uDepthMap", 0);
        m_ssaoShader->SetInt("uNormalMap", 2);
        m_ssaoShader->SetInt("uNoiseMap", 1);
        m_ssaoShader->SetInt(
            "uUseGeometryNormals",
            m_sceneFramebuffer->HasGeometryNormals() ? 1 : 0);
        m_ssaoShader->SetMat4("uProjection", projectionMatrix);
        m_ssaoShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssaoShader->SetFloat("uRadius", m_ssaoRadius);
        m_ssaoShader->SetFloat("uBias", m_ssaoBias);
        m_ssaoShader->SetInt("uKernelSize", KernelSampleCount);
        m_ssaoShader->SetVec3Array("uSamples", m_kernelSamples.data(), KernelSampleCount);
        m_ssaoShader->SetFloat("uNoiseScaleX", noiseScale.x);
        m_ssaoShader->SetFloat("uNoiseScaleY", noiseScale.y);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_noiseTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetNormalColorTexture());
        DrawFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFbo);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_blurShader->Use();
        m_blurShader->SetInt("uInput", 0);
        m_blurShader->SetFloat("uTexelSizeX", texelSize.x);
        m_blurShader->SetFloat("uTexelSizeY", texelSize.y);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);
        DrawFullscreenQuad();
    }

    const bool pbrDebugActive =
        m_debugMode >= RenderDebugMode::ShadowFactor && m_debugMode <= RenderDebugMode::CascadeIndex;
    const bool useShadowFactorComposite = m_sceneFramebuffer->HasShadowFactor() && !pbrDebugActive;

    unsigned int shadowFactorTexture = m_sceneFramebuffer->GetShadowFactorTexture();
    if (useShadowFactorComposite &&
        shadowSettings.GetShadowBlurEnabled() &&
        shadowSettings.GetShadowBlurRadius() > 0.0f)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowBlurFbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_shadowBlurShader->Use();
        m_shadowBlurShader->SetInt("uInput", 0);
        m_shadowBlurShader->SetInt("uDepthMap", 1);
        m_shadowBlurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_shadowBlurShader->SetFloat("uDirectionX", texelSize.x);
        m_shadowBlurShader->SetFloat("uDirectionY", 0.0f);
        m_shadowBlurShader->SetFloat("uBlurRadius", shadowSettings.GetShadowBlurRadius());
        m_shadowBlurShader->SetFloat("uDepthThreshold", 0.08f);
        m_shadowBlurShader->SetFloat("uShadowThreshold", 0.18f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadowFactorTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        DrawFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowBlur2Fbo);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_shadowBlurShader->Use();
        m_shadowBlurShader->SetInt("uInput", 0);
        m_shadowBlurShader->SetInt("uDepthMap", 1);
        m_shadowBlurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_shadowBlurShader->SetFloat("uDirectionX", 0.0f);
        m_shadowBlurShader->SetFloat("uDirectionY", texelSize.y);
        m_shadowBlurShader->SetFloat("uBlurRadius", shadowSettings.GetShadowBlurRadius());
        m_shadowBlurShader->SetFloat("uDepthThreshold", 0.08f);
        m_shadowBlurShader->SetFloat("uShadowThreshold", 0.18f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_shadowBlurTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        DrawFullscreenQuad();

        shadowFactorTexture = m_shadowBlur2Texture;
    }

    std::uintptr_t hdrColorTexture = m_sceneFramebuffer->GetColorTexture();

    if (m_sceneFramebuffer->HasSplitLighting())
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_hdrCompositeFbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_compositeShader->Use();
        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 1);
        m_compositeShader->SetInt("uDepthMap", 3);
        m_compositeShader->SetInt("uSsaoMap", 2);
        m_compositeShader->SetInt("uUseSplitLighting", 1);
        m_compositeShader->SetInt("uUseSsao", runSsao ? 1 : 0);
        m_compositeShader->SetInt("uUseShadowFactor", useShadowFactorComposite ? 1 : 0);
        m_compositeShader->SetInt("uShadowFactorMap", 4);
        m_compositeShader->SetFloat("uSsaoPower", m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_sceneFramebuffer->GetColorTexture()));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetIndirectColorTexture());
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTexture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, shadowFactorTexture);
        DrawFullscreenQuad();

        hdrColorTexture = m_hdrCompositeTexture;
    }
    else if (runSsao)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_hdrCompositeFbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_compositeShader->Use();
        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 0);
        m_compositeShader->SetInt("uDepthMap", 2);
        m_compositeShader->SetInt("uSsaoMap", 1);
        m_compositeShader->SetInt("uUseSplitLighting", 0);
        m_compositeShader->SetInt("uUseSsao", 1);
        m_compositeShader->SetFloat("uSsaoPower", m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_sceneFramebuffer->GetColorTexture()));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        DrawFullscreenQuad();

        hdrColorTexture = m_hdrCompositeTexture;
    }

    unsigned int bloomTexture = 0;
    if (m_bloomEnabled)
    {
        const int bloomWidth = std::max(1, m_width / 2);
        const int bloomHeight = std::max(1, m_height / 2);
        const glm::vec2 bloomTexelSize(
            1.0f / static_cast<float>(bloomWidth),
            1.0f / static_cast<float>(bloomHeight));

        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomExtractFbo);
        glViewport(0, 0, bloomWidth, bloomHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_bloomExtractShader->Use();
        m_bloomExtractShader->SetInt("uHdrColor", 0);
        m_bloomExtractShader->SetFloat("uThreshold", m_bloomThreshold);
        m_bloomExtractShader->SetFloat("uSoftKnee", m_bloomSoftKnee);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
        DrawFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomBlurFbo);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_bloomBlurShader->Use();
        m_bloomBlurShader->SetInt("uInput", 0);
        m_bloomBlurShader->SetFloat("uDirectionX", bloomTexelSize.x);
        m_bloomBlurShader->SetFloat("uDirectionY", 0.0f);
        m_bloomBlurShader->SetFloat("uBlurRadius", m_bloomBlurRadius);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_bloomExtractTexture);
        DrawFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, m_bloomBlur2Fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_bloomBlurShader->Use();
        m_bloomBlurShader->SetInt("uInput", 0);
        m_bloomBlurShader->SetFloat("uDirectionX", 0.0f);
        m_bloomBlurShader->SetFloat("uDirectionY", bloomTexelSize.y);
        m_bloomBlurShader->SetFloat("uBlurRadius", m_bloomBlurRadius);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_bloomBlurTexture);
        DrawFullscreenQuad();

        bloomTexture = m_bloomBlur2Texture;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(0, 0, viewportWidth, viewportHeight);

    if (IsPostProcessDebugMode(m_debugMode))
    {
        unsigned int debugTexture = 0;
        if (m_debugMode == RenderDebugMode::Ssao && runSsao)
        {
            debugTexture = m_ssaoBlurTexture;
        }
        else if (m_debugMode == RenderDebugMode::CompositeOcclusion && runSsao)
        {
            debugTexture = m_hdrCompositeTexture;
        }

        if (debugTexture != 0)
        {
            m_debugChannelShader->Use();
            m_debugChannelShader->SetInt("uInput", 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, debugTexture);
            DrawFullscreenQuad();
            glEnable(GL_DEPTH_TEST);
            glBindTexture(GL_TEXTURE_2D, 0);
            return;
        }
    }

    m_tonemapShader->Use();
    m_tonemapShader->SetInt("uHdrColor", 0);
    m_tonemapShader->SetFloat("uExposure", m_exposure);
    m_tonemapShader->SetInt("uTonemapMode", static_cast<int>(m_tonemapMode));
    m_tonemapShader->SetInt("uUseBloom", m_bloomEnabled ? 1 : 0);
    m_tonemapShader->SetFloat("uBloomIntensity", m_bloomIntensity);
    m_tonemapShader->SetInt("uBloom", 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
    if (m_bloomEnabled)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTexture);
    }
    DrawFullscreenQuad();

    glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool ScreenSpaceEffects::IsEnabled() const
{
    return m_enabled;
}

void ScreenSpaceEffects::SetEnabled(bool enabled)
{
    m_enabled = enabled;
}

bool ScreenSpaceEffects::IsSsaoEnabled() const
{
    return m_ssaoEnabled;
}

void ScreenSpaceEffects::SetSsaoEnabled(bool enabled)
{
    m_ssaoEnabled = enabled;
}

float ScreenSpaceEffects::GetSsaoRadius() const
{
    return m_ssaoRadius;
}

void ScreenSpaceEffects::SetSsaoRadius(float radius)
{
    m_ssaoRadius = std::max(radius, 0.01f);
}

float ScreenSpaceEffects::GetSsaoBias() const
{
    return m_ssaoBias;
}

void ScreenSpaceEffects::SetSsaoBias(float bias)
{
    m_ssaoBias = std::max(bias, 0.0f);
}

float ScreenSpaceEffects::GetSsaoPower() const
{
    return m_ssaoPower;
}

void ScreenSpaceEffects::SetSsaoPower(float power)
{
    m_ssaoPower = std::max(power, 0.1f);
}

float ScreenSpaceEffects::GetAoStrength() const
{
    return m_aoStrength;
}

void ScreenSpaceEffects::SetAoStrength(float strength)
{
    m_aoStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetExposure() const
{
    return m_exposure;
}

void ScreenSpaceEffects::SetExposure(float exposure)
{
    m_exposure = std::clamp(exposure, -4.0f, 6.0f);
}

TonemapMode ScreenSpaceEffects::GetTonemapMode() const
{
    return m_tonemapMode;
}

void ScreenSpaceEffects::SetTonemapMode(TonemapMode mode)
{
    m_tonemapMode = mode;
}

bool ScreenSpaceEffects::IsBloomEnabled() const
{
    return m_bloomEnabled;
}

void ScreenSpaceEffects::SetBloomEnabled(bool enabled)
{
    m_bloomEnabled = enabled;
}

float ScreenSpaceEffects::GetBloomThreshold() const
{
    return m_bloomThreshold;
}

void ScreenSpaceEffects::SetBloomThreshold(float threshold)
{
    m_bloomThreshold = std::max(threshold, 0.0f);
}

float ScreenSpaceEffects::GetBloomSoftKnee() const
{
    return m_bloomSoftKnee;
}

void ScreenSpaceEffects::SetBloomSoftKnee(float softKnee)
{
    m_bloomSoftKnee = std::clamp(softKnee, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetBloomIntensity() const
{
    return m_bloomIntensity;
}

void ScreenSpaceEffects::SetBloomIntensity(float intensity)
{
    m_bloomIntensity = std::max(intensity, 0.0f);
}

float ScreenSpaceEffects::GetBloomBlurRadius() const
{
    return m_bloomBlurRadius;
}

void ScreenSpaceEffects::SetBloomBlurRadius(float blurRadius)
{
    m_bloomBlurRadius = std::clamp(blurRadius, 0.25f, 4.0f);
}

RenderDebugMode ScreenSpaceEffects::GetDebugMode() const
{
    return m_debugMode;
}

void ScreenSpaceEffects::SetDebugMode(const RenderDebugMode mode)
{
    m_debugMode = mode;
}

void ScreenSpaceEffects::BlitDepthToFramebuffer(
    std::uintptr_t drawFramebuffer,
    int viewportWidth,
    int viewportHeight) const
{
    if (!m_sceneFramebuffer->IsValid())
    {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(m_sceneFramebuffer->GetFramebuffer()));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
    glBlitFramebuffer(
        0,
        0,
        m_width,
        m_height,
        0,
        0,
        viewportWidth,
        viewportHeight,
        GL_DEPTH_BUFFER_BIT,
        GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
    glViewport(0, 0, viewportWidth, viewportHeight);
}
