#pragma once

#include "app/scene/Scene.h"
#include "app/scene/RenderViewport.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/SceneLighting.h"

#include "engine/rendering/TextureSamplerSettings.h"

#include "engine/rendering/DxrSettings.h"

#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrGiDispatch.h"
#include "engine/raytracing/DxrPrimaryDebugDispatch.h"
#include "engine/raytracing/DxrPathTracerDispatch.h"
#include "engine/raytracing/DxrReflectionsDispatch.h"
#include "engine/raytracing/DxrShadowsDispatch.h"
#include "engine/raytracing/DxrSmokeDispatch.h"

#include "engine/rendering/RenderDebug.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Camera;
class CameraGizmoRenderer;
class CascadedShadowMap;
class ColliderGizmoRenderer;
class Framebuffer;
class GridRenderer;
class EnvironmentMap;
class IBL;
class LightGizmoRenderer;
class ScreenSpaceEffects;
class Shader;

class SceneRenderer
{
    enum class GpuResourceState
    {
        NotStarted,
        InProgress,
        Ready,
        Failed,
    };

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
        const SceneRenderOptions& options,
        RenderViewport renderViewport = RenderViewport::SceneView);

    const SceneLighting& GetLighting() const;
    SceneLighting& GetLighting();

    IBL& GetIBL();
    const IBL& GetIBL() const;

    EnvironmentMap& GetEnvironmentMap();
    const EnvironmentMap& GetEnvironmentMap() const;

    ScreenSpaceEffects& GetScreenSpaceEffects();
    const ScreenSpaceEffects& GetScreenSpaceEffects() const;

    void SetRenderDebugMode(RenderDebugMode mode);
    RenderDebugMode GetRenderDebugMode() const;

    DxrSettings& GetDxrSettings();
    const DxrSettings& GetDxrSettings() const;

    const DxrDiagnostics& GetDxrDiagnostics() const;

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

    bool IsGpuResourcesReady() const { return m_gpuResourceState == GpuResourceState::Ready; }
    bool HasGpuResourcesInitFailed() const { return m_gpuResourceState == GpuResourceState::Failed; }
    const std::string& GetGpuResourcesInitError() const { return m_gpuResourcesInitError; }
    void PrepareGpuResources() const { EnsureGpuResources(); }
    void PrepareGpuResourcesForGeometryMsaa(int msaaSampleCount) const;
    void ResetGpuResourcesIfInitFailed() const;
    bool ApplyGeometryMsaaReload(Scene& scene, int viewportWidth, int viewportHeight, std::string* outError = nullptr);

    // Geometry MSAA reload recreates GPU pipelines/framebuffers, so it must run at a frame boundary
    // (before ImGui builds draw data that references soon-to-be-destroyed resources), never mid-UI.
    // The editor requests a reload here; Application applies it from ProcessPendingGpuStructuralChanges.
    void RequestGeometryMsaaReload() { m_geometryMsaaReloadRequested = true; }
    bool IsGeometryMsaaReloadRequested() const { return m_geometryMsaaReloadRequested; }
    bool HasGeometryMsaaReloadFailed() const { return m_geometryMsaaReloadFailed; }
    const std::string& GetGeometryMsaaReloadError() const { return m_geometryMsaaReloadError; }

    void MergePendingRendererSettings(const nlohmann::json& delta);
    bool HasPendingRendererSettings() const { return m_hasPendingRendererSettings; }
    nlohmann::json TakePendingRendererSettings();

    void WarmUpDxrPipelineIfNeeded();
    void PrepareFrameGpuResources();
    // Must run before BeginFrame — constructing ScreenSpaceEffects uploads shaders via ExecuteImmediate.
    void PrepareGameViewGpuResources();
    void InvalidateGameViewMotionOnPlayStop();

private:
    [[noreturn]] void ThrowGpuResourcesUnavailable() const;
    void EnsureGpuResources() const;
    void ResetPartialGpuResources() const;
    void SyncLighting(const Scene& scene);
    glm::vec3 GetSunDirection() const;
    void RenderShadowPass(const Scene& scene, const Camera& camera);
    void PrepareSceneRasterTarget(
        const Scene& scene,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        Framebuffer* target,
        bool usePostProcess,
        bool freezeTemporalJitter,
        bool& outSplitLightingMrt);
    void RenderGeometryPass(
        const Scene& scene,
        const Camera& camera,
        bool usePostProcess,
        Framebuffer* target,
        bool splitLightingMrt,
        RenderDebugMode materialDebugMode,
        RenderDebugMode activeDebugMode);
    void RenderPostProcessPass(
        const Scene& scene,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        Framebuffer* target,
        const SceneRenderOptions& options,
        bool freezeTemporalJitter,
        bool splitLightingMrt);
    void RenderGizmoPass(
        const Scene& scene,
        const Camera& camera,
        Framebuffer* target,
        const SceneRenderOptions& options,
        bool usePostProcess);
    void RecordDxrPass(
        const Scene& scene,
        const Camera& camera,
        int dispatchWidth,
        int dispatchHeight,
        std::uintptr_t depthSrvCpuHandle,
        bool usePostProcess);
    void SyncEffectiveMaterialMipBias() const;
    void EnsureGameViewScreenSpaceEffects();
    void SyncGameViewScreenSpaceSettings();
    void MaybeInvalidateStaleViewportTemporalState(RenderViewport viewport);
    void InvalidateViewportTemporalState(RenderViewport viewport);

    std::unique_ptr<CameraGizmoRenderer> m_cameraGizmos;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<ColliderGizmoRenderer> m_colliderGizmos;
    std::unique_ptr<LightGizmoRenderer> m_lightGizmos;
    std::unique_ptr<Shader> m_shadowDepthShader;
    std::unique_ptr<CascadedShadowMap> m_shadowMap;
    std::unique_ptr<EnvironmentMap> m_environmentMap;
    std::unique_ptr<ScreenSpaceEffects> m_screenSpaceEffects;
    std::unique_ptr<ScreenSpaceEffects> m_gameViewScreenSpaceEffects;
    ScreenSpaceEffects* m_activeScreenSpaceEffects = nullptr;
    std::unique_ptr<DxrAccelerationStructures> m_dxrAccelerationStructures;
    std::unique_ptr<DxrSmokeDispatch> m_dxrSmokeDispatch;
    std::unique_ptr<DxrPrimaryDebugDispatch> m_dxrPrimaryDebugDispatch;
    std::unique_ptr<DxrPathTracerDispatch> m_dxrPathTracerDispatch;
    std::unique_ptr<DxrReflectionsDispatch> m_dxrReflectionsDispatch;
    std::unique_ptr<DxrShadowsDispatch> m_dxrShadowsDispatch;
    std::unique_ptr<DxrGiDispatch> m_dxrGiDispatch;
    DxrSettings m_dxrSettings;
    DirectionalShadowSettings m_directionalShadowSettings;
    TextureFilterMode m_textureFilterMode = TextureFilterMode::Trilinear;
    std::uint32_t m_textureAnisotropy = 8;
    float m_textureMipBias = 0.0f;
    SceneLighting m_lighting;
    mutable std::vector<glm::mat4> m_previousWorldMatrices;
    mutable std::vector<glm::mat4> m_gameViewPreviousWorldMatrices;
    std::uint64_t m_sceneViewLastSubmissionFrame = 0;
    std::uint64_t m_gameViewLastSubmissionFrame = 0;
    std::vector<glm::mat4>* m_activePreviousWorldMatrices = nullptr;
    mutable GpuResourceState m_gpuResourceState = GpuResourceState::NotStarted;
    mutable std::string m_gpuResourcesInitError;
    nlohmann::json m_pendingRendererSettings;
    bool m_hasPendingRendererSettings = false;
    bool m_geometryMsaaReloadRequested = false;
    bool m_geometryMsaaReloadFailed = false;
    std::string m_geometryMsaaReloadError;
};
