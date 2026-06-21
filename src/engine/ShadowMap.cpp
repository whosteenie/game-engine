#include "engine/ShadowMap.h"

#include <glad/glad.h>

#include <glm/gtc/matrix_transform.hpp>

#include <stdexcept>

ShadowMap::ShadowMap(int resolution)
    : m_resolution(resolution)
{
    CreateResources();
}

ShadowMap::~ShadowMap()
{
    glDeleteFramebuffers(1, &m_fbo);
    glDeleteTextures(1, &m_depthTexture);
}

void ShadowMap::CreateResources()
{
    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_depthTexture);

    glBindTexture(GL_TEXTURE_2D, m_depthTexture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_DEPTH_COMPONENT,
        m_resolution,
        m_resolution,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

    const float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        throw std::runtime_error("Shadow framebuffer is incomplete");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowMap::BeginPass(const glm::vec3& lightDirectionTowardSource, const glm::vec3& sceneCenter)
{
    const float orthoExtent = 14.0f;
    const float nearPlane = 1.0f;
    const float farPlane = 30.0f;

    const glm::mat4 lightProjection = glm::ortho(
        -orthoExtent,
        orthoExtent,
        -orthoExtent,
        orthoExtent,
        nearPlane,
        farPlane);

    const glm::vec3 normalizedLightDirection = glm::normalize(lightDirectionTowardSource);
    const glm::vec3 lightEye = sceneCenter + normalizedLightDirection * 18.0f;
    const glm::mat4 lightView = glm::lookAt(lightEye, sceneCenter, glm::vec3(0.0f, 1.0f, 0.0f));

    m_lightSpaceMatrix = lightProjection * lightView;

    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_FRONT);
}

void ShadowMap::EndPass()
{
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

const glm::mat4& ShadowMap::GetLightSpaceMatrix() const
{
    return m_lightSpaceMatrix;
}

void ShadowMap::BindDepthTexture(unsigned int textureUnit) const
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_depthTexture);
}
