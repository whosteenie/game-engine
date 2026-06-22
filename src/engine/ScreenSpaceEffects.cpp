#include <glad/glad.h>

#include "engine/ScreenSpaceEffects.h"

#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Framebuffer.h"
#include "engine/Shader.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace
{
    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };
}

ScreenSpaceEffects::ScreenSpaceEffects()
    : m_sceneFramebuffer(std::make_unique<Framebuffer>()),
      m_ssaoShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoFragmentShader)),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoBlurFragmentShader)),
      m_contactShadowShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ContactShadowFragmentShader)),
      m_compositeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ScreenCompositeFragmentShader))
{
    CreateFullscreenQuad();
    CreateNoiseTexture();
    CreateKernel();
}

ScreenSpaceEffects::~ScreenSpaceEffects()
{
    DestroySingleChannelTarget(m_ssaoFbo, m_ssaoTexture);
    DestroySingleChannelTarget(m_ssaoBlurFbo, m_ssaoBlurTexture);
    DestroySingleChannelTarget(m_contactFbo, m_contactTexture);
    DestroySingleChannelTarget(m_contactBlurFbo, m_contactBlurTexture);

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
    DestroySingleChannelTarget(m_contactFbo, m_contactTexture);
    DestroySingleChannelTarget(m_contactBlurFbo, m_contactBlurTexture);

    CreateSingleChannelTarget(m_ssaoFbo, m_ssaoTexture, width, height);
    CreateSingleChannelTarget(m_ssaoBlurFbo, m_ssaoBlurTexture, width, height);
    CreateSingleChannelTarget(m_contactFbo, m_contactTexture, width, height);
    CreateSingleChannelTarget(m_contactBlurFbo, m_contactBlurTexture, width, height);
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

    m_sceneFramebuffer->Resize(width, height);
    ResizeSingleChannelTargets(width, height);
    m_width = width;
    m_height = height;
}

void ScreenSpaceEffects::BeginScenePass() const
{
    m_sceneFramebuffer->Bind();
    glClearColor(0.08f, 0.09f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
    const glm::vec3& lightDirection,
    int viewportWidth,
    int viewportHeight) const
{
    if (!m_enabled || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    const bool runSsao = m_ssaoEnabled;
    const bool runContactShadows = m_contactShadowsEnabled;
    if (!runSsao && !runContactShadows)
    {
        return;
    }

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 viewMatrix = camera.GetViewMatrix();
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
        m_ssaoShader->SetInt("uNoiseMap", 1);
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

    if (runContactShadows)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_contactFbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_contactShadowShader->Use();
        m_contactShadowShader->SetInt("uDepthMap", 0);
        m_contactShadowShader->SetMat4("uProjection", projectionMatrix);
        m_contactShadowShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_contactShadowShader->SetMat4("uView", viewMatrix);
        m_contactShadowShader->SetVec3("uLightDirection", glm::normalize(lightDirection));
        m_contactShadowShader->SetFloat("uMaxDistance", m_contactShadowDistance);
        m_contactShadowShader->SetInt("uStepCount", m_contactShadowSteps);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetDepthTexture());
        DrawFullscreenQuad();

        glBindFramebuffer(GL_FRAMEBUFFER, m_contactBlurFbo);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        m_blurShader->Use();
        m_blurShader->SetInt("uInput", 0);
        m_blurShader->SetFloat("uTexelSizeX", texelSize.x);
        m_blurShader->SetFloat("uTexelSizeY", texelSize.y);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_contactTexture);
        DrawFullscreenQuad();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(0, 0, viewportWidth, viewportHeight);

    m_compositeShader->Use();
    m_compositeShader->SetInt("uSceneColor", 0);
    m_compositeShader->SetInt("uSsaoMap", 1);
    m_compositeShader->SetInt("uContactShadowMap", 2);
    m_compositeShader->SetInt("uUseSsao", runSsao ? 1 : 0);
    m_compositeShader->SetInt("uUseContactShadows", runContactShadows ? 1 : 0);
    m_compositeShader->SetFloat("uSsaoPower", m_ssaoPower);
    m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
    m_compositeShader->SetFloat("uContactStrength", m_contactStrength);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_sceneFramebuffer->GetColorTexture());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_ssaoBlurTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_contactBlurTexture);
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

bool ScreenSpaceEffects::IsContactShadowsEnabled() const
{
    return m_contactShadowsEnabled;
}

void ScreenSpaceEffects::SetContactShadowsEnabled(bool enabled)
{
    m_contactShadowsEnabled = enabled;
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

float ScreenSpaceEffects::GetContactShadowDistance() const
{
    return m_contactShadowDistance;
}

void ScreenSpaceEffects::SetContactShadowDistance(float distance)
{
    m_contactShadowDistance = std::max(distance, 0.01f);
}

int ScreenSpaceEffects::GetContactShadowSteps() const
{
    return m_contactShadowSteps;
}

void ScreenSpaceEffects::SetContactShadowSteps(int steps)
{
    m_contactShadowSteps = std::clamp(steps, 4, 32);
}

float ScreenSpaceEffects::GetAoStrength() const
{
    return m_aoStrength;
}

void ScreenSpaceEffects::SetAoStrength(float strength)
{
    m_aoStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetContactStrength() const
{
    return m_contactStrength;
}

void ScreenSpaceEffects::SetContactStrength(float strength)
{
    m_contactStrength = std::clamp(strength, 0.0f, 1.0f);
}

void ScreenSpaceEffects::BlitDepthToDefaultFramebuffer(int viewportWidth, int viewportHeight) const
{
    if (!m_sceneFramebuffer->IsValid())
    {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFramebuffer->GetFramebuffer());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
