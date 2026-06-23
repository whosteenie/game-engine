#include "engine/CascadedShadowMap.h"

#include "engine/Camera.h"
#include "engine/ShadowMapMath.h"

#include <glad/glad.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <stdexcept>

CascadedShadowMap::CascadedShadowMap(const int resolutionPerCascade)
    : m_resolution(resolutionPerCascade)
{
    CreateResources();
}

CascadedShadowMap::~CascadedShadowMap()
{
    glDeleteFramebuffers(1, &m_fbo);
    glDeleteTextures(1, &m_depthTexture);
}

void CascadedShadowMap::CreateResources()
{
    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_depthTexture);

    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTexture);
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_DEPTH_COMPONENT,
        m_resolution,
        m_resolution,
        CascadeCount,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

    const float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTextureLayer(
        GL_FRAMEBUFFER,
        GL_DEPTH_ATTACHMENT,
        m_depthTexture,
        0,
        0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        throw std::runtime_error("Cascaded shadow framebuffer is incomplete");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CascadedShadowMap::BeginFrame(
    const Camera& camera,
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& sceneBoundsMin,
    const glm::vec3& sceneBoundsMax)
{
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);

    const float shadowDrawDistance = ComputeShadowDrawDistance(
        camera.GetPosition(),
        sceneBoundsMin,
        sceneBoundsMax);
    const float cascadeFarPlane = std::min(camera.GetFarPlane(), shadowDrawDistance);

    const std::vector<float> splitDistances = ComputeCascadeSplitDistances(
        CascadeCount,
        camera.GetNearPlane(),
        cascadeFarPlane,
        0.75f);

    const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());

    for (int cascadeIndex = 0; cascadeIndex < CascadeCount; ++cascadeIndex)
    {
        const float cascadeNear = splitDistances[static_cast<std::size_t>(cascadeIndex)];
        const float cascadeFar = splitDistances[static_cast<std::size_t>(cascadeIndex + 1)];
        m_cascadeEndSplits[static_cast<std::size_t>(cascadeIndex)] = cascadeFar;

        const std::array<glm::vec3, 8> frustumCorners = ComputeCascadeFrustumCorners(
            inverseViewMatrix,
            camera.GetAspect(),
            camera.GetFov(),
            cascadeNear,
            cascadeFar);

        const glm::vec3 frustumMin = ComputeBoundsMin(frustumCorners);
        const glm::vec3 frustumMax = ComputeBoundsMax(frustumCorners);

        m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)] = BuildShadowLightSpace(
            lightDirectionTowardSource,
            frustumMin,
            frustumMax,
            m_resolution);
        m_lightSpaceMatrices[static_cast<std::size_t>(cascadeIndex)] =
            m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)].lightSpaceMatrix;
    }
}

void CascadedShadowMap::BeginCascade(const int cascadeIndex)
{
    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTextureLayer(
        GL_FRAMEBUFFER,
        GL_DEPTH_ATTACHMENT,
        m_depthTexture,
        0,
        cascadeIndex);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 2.0f);
    glCullFace(GL_FRONT);
}

void CascadedShadowMap::RestoreRasterState() const
{
    glDisable(GL_POLYGON_OFFSET_FILL);
    glCullFace(GL_BACK);
    glViewport(
        m_savedViewport[0],
        m_savedViewport[1],
        m_savedViewport[2],
        m_savedViewport[3]);
}

void CascadedShadowMap::EndFrame()
{
    RestoreRasterState();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

const glm::mat4& CascadedShadowMap::GetLightSpaceMatrix(const int cascadeIndex) const
{
    return m_lightSpaceMatrices[static_cast<std::size_t>(cascadeIndex)];
}

const std::array<glm::mat4, CascadedShadowMap::CascadeCount>& CascadedShadowMap::GetLightSpaceMatrices() const
{
    return m_lightSpaceMatrices;
}

const std::array<ShadowLightSpaceSetup, CascadedShadowMap::CascadeCount>& CascadedShadowMap::GetCascadeSetups() const
{
    return m_cascadeSetups;
}

const std::array<float, CascadedShadowMap::CascadeCount>& CascadedShadowMap::GetCascadeEndSplits() const
{
    return m_cascadeEndSplits;
}

int CascadedShadowMap::GetResolution() const
{
    return m_resolution;
}

void CascadedShadowMap::BindDepthTexture(const unsigned int textureUnit) const
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTexture);
}
