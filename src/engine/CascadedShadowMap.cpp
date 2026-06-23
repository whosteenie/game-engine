#include "engine/CascadedShadowMap.h"

#include "engine/Camera.h"
#include "engine/ShadowMapMath.h"

#include <glad/glad.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

CascadedShadowMap::CascadedShadowMap(const int resolutionPerCascade)
    : m_resolution(resolutionPerCascade)
{
    CreateResources();
}

CascadedShadowMap::~CascadedShadowMap()
{
    DestroyResources();
}

void CascadedShadowMap::DestroyResources()
{
    if (m_depthTexture != 0)
    {
        glDeleteTextures(1, &m_depthTexture);
        m_depthTexture = 0;
    }

    if (m_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
}

void CascadedShadowMap::CreateResources()
{
    DestroyResources();

    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_depthTexture);

    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTexture);
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_DEPTH_COMPONENT32F,
        m_resolution,
        m_resolution,
        MaxCascades,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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

void CascadedShadowMap::SetResolution(const int resolutionPerCascade)
{
    const int clampedResolution = std::clamp(resolutionPerCascade, 512, 8192);
    if (clampedResolution == m_resolution)
    {
        return;
    }

    m_resolution = clampedResolution;
    CreateResources();
}

void CascadedShadowMap::BeginFrame(
    const Camera& camera,
    const glm::vec3& lightDirectionTowardSource,
    const glm::vec3& casterBoundsMin,
    const glm::vec3& casterBoundsMax,
    const bool hasCasterBounds,
    const DirectionalShadowSettings& settings)
{
    m_activeCascadeCount = settings.GetCascadeCount();
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);

    const float shadowDrawDistance = hasCasterBounds
        ? ComputeShadowDrawDistance(camera.GetPosition(), casterBoundsMin, casterBoundsMax)
        : camera.GetFarPlane();
    const float cascadeFarPlane = std::min(camera.GetFarPlane(), shadowDrawDistance);

    const std::vector<float> splitDistances = ComputeCascadeSplitDistances(
        m_activeCascadeCount,
        camera.GetNearPlane(),
        cascadeFarPlane,
        settings.GetCascadeSplitLambda());

    const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());
    const glm::vec3 cameraPosition = camera.GetPosition();
    const float cameraMoveDistance = m_hasStableOrthoHalfExtents
        ? glm::length(cameraPosition - m_lastCameraPosition)
        : std::numeric_limits<float>::max();
    const bool allowOrthoShrink = cameraMoveDistance > 0.75f;
    if (allowOrthoShrink)
    {
        m_stableOrthoHalfExtents.fill(0.0f);
        m_hasStableOrthoHalfExtents = false;
    }

    for (int cascadeIndex = 0; cascadeIndex < m_activeCascadeCount; ++cascadeIndex)
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

        m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)] = BuildShadowLightSpaceForFrustumCorners(
            lightDirectionTowardSource,
            frustumCorners,
            m_resolution,
            settings.GetXyMarginFraction(),
            settings.GetZMarginFraction(),
            hasCasterBounds ? &casterBoundsMin : nullptr,
            hasCasterBounds ? &casterBoundsMax : nullptr,
            settings.GetTightNearPlaneXyFit(),
            &cameraPosition,
            &m_stableOrthoHalfExtents[static_cast<std::size_t>(cascadeIndex)],
            allowOrthoShrink);
        m_lightSpaceMatrices[static_cast<std::size_t>(cascadeIndex)] =
            m_cascadeSetups[static_cast<std::size_t>(cascadeIndex)].lightSpaceMatrix;
    }

    m_lastCameraPosition = cameraPosition;
    m_hasStableOrthoHalfExtents = true;
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

const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetLightSpaceMatrices() const
{
    return m_lightSpaceMatrices;
}

const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetCascadeSetups() const
{
    return m_cascadeSetups;
}

const std::array<float, CascadedShadowMap::MaxCascades>& CascadedShadowMap::GetCascadeEndSplits() const
{
    return m_cascadeEndSplits;
}

int CascadedShadowMap::GetResolution() const
{
    return m_resolution;
}

int CascadedShadowMap::GetActiveCascadeCount() const
{
    return m_activeCascadeCount;
}

void CascadedShadowMap::BindDepthTexture(const unsigned int textureUnit) const
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTexture);
}
