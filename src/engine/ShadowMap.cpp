#include "engine/ShadowMap.h"
#include "engine/ShadowMapMath.h"

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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
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

void ShadowMap::BeginPass(
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax)
{
    const ShadowLightSpaceSetup setup = BuildShadowLightSpace(
        lightDirectionTowardSource,
        boundsMin,
        boundsMax,
        m_resolution);
    m_lightSpaceMatrix = setup.lightSpaceMatrix;

    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 2.0f);
    glCullFace(GL_FRONT);
}

void ShadowMap::EndPass()
{
    glDisable(GL_POLYGON_OFFSET_FILL);
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
