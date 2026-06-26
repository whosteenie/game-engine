#pragma once

#include "app/scene/Scene.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/SceneLighting.h"

#include "engine/rendering/TextureSamplerSettings.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>

class Camera;
class CameraGizmoRenderer;
class CascadedShadowMap;
class ColliderGizmoRenderer;
class GridRenderer;
class IBL;
class LightGizmoRenderer;
class ScreenSpaceEffects;
class Shader;

class SceneRenderer
{
public:
    SceneRenderer();
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Render(
        const Scene& scene,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        std::uintptr_t targetFramebuffer,
        const SceneRenderOptions& options);

    const SceneLighting& GetLighting() const;
    SceneLighting& GetLighting();

    IBL& GetIBL();
    const IBL& GetIBL() const;

    ScreenSpaceEffects& GetScreenSpaceEffects();
    const ScreenSpaceEffects& GetScreenSpaceEffects() const;

    bool ComputeShadowCasterBounds(
        const Scene& scene,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax) const;

    const DirectionalShadowSettings& GetDirectionalShadowSettings() const;
    DirectionalShadowSettings& GetDirectionalShadowSettings();

    const CascadedShadowMap& GetShadowMap() const;

    TextureFilterMode GetTextureFilterMode() const { return m_textureFilterMode; }
    void SetTextureFilterMode(TextureFilterMode mode);

    std::uint32_t GetTextureAnisotropy() const { return m_textureAnisotropy; }
    void SetTextureAnisotropy(std::uint32_t anisotropy);

    float GetTextureMipBias() const { return m_textureMipBias; }
    void SetTextureMipBias(float mipBias);

    bool IsGpuResourcesReady() const { return m_gpuResourcesInitialized; }
    bool HasGpuResourcesInitFailed() const { return m_gpuResourcesInitFailed; }
    const std::string& GetGpuResourcesInitError() const { return m_gpuResourcesInitError; }
    void PrepareGpuResources() const { EnsureGpuResources(); }

private:
    [[noreturn]] void ThrowGpuResourcesUnavailable() const;
    void EnsureGpuResources() const;
    void SyncLighting(const Scene& scene);
    glm::vec3 GetSunDirection() const;
    void RenderShadowPass(const Scene& scene, const Camera& camera);

    std::unique_ptr<CameraGizmoRenderer> m_cameraGizmos;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<ColliderGizmoRenderer> m_colliderGizmos;
    std::unique_ptr<LightGizmoRenderer> m_lightGizmos;
    std::unique_ptr<Shader> m_shadowDepthShader;
    std::unique_ptr<CascadedShadowMap> m_shadowMap;
    std::unique_ptr<IBL> m_ibl;
    std::unique_ptr<ScreenSpaceEffects> m_screenSpaceEffects;
    DirectionalShadowSettings m_directionalShadowSettings;
    TextureFilterMode m_textureFilterMode = TextureFilterMode::Trilinear;
    std::uint32_t m_textureAnisotropy = 8;
    float m_textureMipBias = 0.0f;
    SceneLighting m_lighting;
    mutable bool m_gpuResourcesInitialized = false;
    mutable bool m_gpuResourcesInitFailed = false;
    mutable std::string m_gpuResourcesInitError;
};
