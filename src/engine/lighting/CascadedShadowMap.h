#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/ShadowMapMath.h"

#include <array>
#include <glm/glm.hpp>

class Camera;

class CascadedShadowMap
{
public:
    static constexpr int MaxCascades = DirectionalShadowSettings::MaxCascades;
    static constexpr int DefaultResolution = 4096;

    explicit CascadedShadowMap(int resolutionPerCascade = DefaultResolution);
    ~CascadedShadowMap();

    void SetResolution(int resolutionPerCascade);

    void BeginFrame(
        const Camera& camera,
        const glm::vec3& lightDirectionTowardSource,
        const glm::vec3& casterBoundsMin,
        const glm::vec3& casterBoundsMax,
        bool hasCasterBounds,
        const DirectionalShadowSettings& settings);

    void BeginCascade(int cascadeIndex);
    void EndFrame();

    const glm::mat4& GetLightSpaceMatrix(int cascadeIndex) const;
    const std::array<glm::mat4, MaxCascades>& GetLightSpaceMatrices() const;
    const std::array<ShadowLightSpaceSetup, MaxCascades>& GetCascadeSetups() const;
    const std::array<float, MaxCascades>& GetCascadeEndSplits() const;
    int GetResolution() const;
    int GetActiveCascadeCount() const;

    void BindDepthTexture(unsigned int textureUnit) const;

private:
    void CreateResources();
    void DestroyResources();
    void RestoreRasterState() const;

    int m_resolution;
    int m_activeCascadeCount = MaxCascades;
    unsigned int m_fbo = 0;
    unsigned int m_depthTexture = 0;
    std::array<glm::mat4, MaxCascades> m_lightSpaceMatrices{};
    std::array<ShadowLightSpaceSetup, MaxCascades> m_cascadeSetups{};
    std::array<float, MaxCascades> m_cascadeEndSplits{};
    std::array<float, MaxCascades> m_stableOrthoHalfExtents{};
    std::array<glm::vec2, MaxCascades> m_stableOrthoCentersLight{};
    std::array<float, MaxCascades> m_stableOrthoZNear{};
    std::array<float, MaxCascades> m_stableOrthoZFar{};
    glm::vec3 m_lastCameraPosition{0.0f};
    bool m_hasStableOrthoHalfExtents = false;
    int m_savedViewport[4]{0, 0, 0, 0};
};
