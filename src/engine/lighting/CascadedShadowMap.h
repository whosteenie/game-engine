#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/ShadowMapMath.h"

#include <array>
#include <cstdint>
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
    bool HasRenderedDepth() const;

    // Shader-visible SRV heap index of the cascade depth Texture2DArray, for raw-D3D12 passes
    // (e.g. the mesh-shader G-buffer lighting) that bind it via a descriptor table rather than the
    // Shader texture-unit path. UINT32_MAX until resources are created.
    std::uint32_t GetDepthSrvIndex() const { return m_depthSrvIndex; }

private:
    void CreateResources();
    void DestroyResources();
    void RestoreRasterState() const;

    int m_resolution;
    int m_activeCascadeCount = 1;
    std::array<glm::mat4, MaxCascades> m_lightSpaceMatrices{};
    std::array<ShadowLightSpaceSetup, MaxCascades> m_cascadeSetups{};
    std::array<float, MaxCascades> m_cascadeEndSplits{};
    bool m_hasRenderedDepth = false;

    void* m_depthResource = nullptr;
    void* m_depthAllocation = nullptr;
    std::uint32_t m_depthSrvIndex = UINT32_MAX;
    std::uintptr_t m_depthSrvCpuHandle = 0;
    std::array<std::uint32_t, MaxCascades> m_dsvIndices{};
    float m_savedViewportWidth = 0.0f;
    float m_savedViewportHeight = 0.0f;
    bool m_inShadowPass = false;
    bool m_depthInShaderReadState = false;
};
