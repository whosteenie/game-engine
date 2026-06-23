#pragma once

#include "engine/ShadowMapMath.h"

#include <array>
#include <glm/glm.hpp>

class Camera;

class CascadedShadowMap
{
public:
    static constexpr int CascadeCount = 4;
    static constexpr int DefaultResolution = 4096;
    static constexpr float CascadeBlendRatio = 0.25f;

    explicit CascadedShadowMap(int resolutionPerCascade = DefaultResolution);
    ~CascadedShadowMap();

    void BeginFrame(
        const Camera& camera,
        const glm::vec3& lightDirectionTowardSource,
        const glm::vec3& sceneBoundsMin,
        const glm::vec3& sceneBoundsMax);

    void BeginCascade(int cascadeIndex);
    void EndFrame();

    const glm::mat4& GetLightSpaceMatrix(int cascadeIndex) const;
    const std::array<glm::mat4, CascadeCount>& GetLightSpaceMatrices() const;
    const std::array<ShadowLightSpaceSetup, CascadeCount>& GetCascadeSetups() const;
    const std::array<float, CascadeCount>& GetCascadeEndSplits() const;
    int GetResolution() const;

    void BindDepthTexture(unsigned int textureUnit) const;

private:
    void CreateResources();
    void RestoreRasterState() const;

    int m_resolution;
    unsigned int m_fbo = 0;
    unsigned int m_depthTexture = 0;
    std::array<glm::mat4, CascadeCount> m_lightSpaceMatrices{};
    std::array<ShadowLightSpaceSetup, CascadeCount> m_cascadeSetups{};
    std::array<float, CascadeCount> m_cascadeEndSplits{};
    int m_savedViewport[4]{0, 0, 0, 0};
};
