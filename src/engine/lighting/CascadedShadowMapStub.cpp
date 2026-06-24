#include "engine/lighting/CascadedShadowMap.h"

#include "engine/camera/Camera.h"

CascadedShadowMap::CascadedShadowMap(int resolutionPerCascade)
    : m_resolution(resolutionPerCascade)
{
}

CascadedShadowMap::~CascadedShadowMap() = default;

void CascadedShadowMap::SetResolution(int resolutionPerCascade)
{
    m_resolution = resolutionPerCascade;
}

void CascadedShadowMap::BeginFrame(
    const Camera& /*camera*/,
    const glm::vec3& /*lightDirectionTowardSource*/,
    const glm::vec3& /*casterBoundsMin*/,
    const glm::vec3& /*casterBoundsMax*/,
    bool /*hasCasterBounds*/,
    const DirectionalShadowSettings& /*settings*/)
{
}

void CascadedShadowMap::BeginCascade(int /*cascadeIndex*/)
{
}

void CascadedShadowMap::EndFrame()
{
}

const glm::mat4& CascadedShadowMap::GetLightSpaceMatrix(int cascadeIndex) const
{
    return m_lightSpaceMatrices.at(static_cast<std::size_t>(cascadeIndex));
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

void CascadedShadowMap::BindDepthTexture(unsigned int /*textureUnit*/) const
{
}

bool CascadedShadowMap::HasRenderedDepth() const
{
    return false;
}
