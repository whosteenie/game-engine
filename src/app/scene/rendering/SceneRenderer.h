#pragma once

#include "engine/rendering/scene/GpuScene.h"
#include "app/scene/document/Scene.h"
#include "app/scene/rendering/RenderViewport.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/SceneLighting.h"

#include "engine/rendering/resources/TextureSamplerSettings.h"

#include "engine/rendering/core/DxrSettings.h"

#include "engine/raytracing/acceleration/DxrAccelerationStructures.h"
#include "engine/raytracing/core/DxrDiagnostics.h"
#include "engine/raytracing/dispatch/DxrGiDispatch.h"
#include "engine/raytracing/dispatch/DxrPrimaryDebugDispatch.h"
#include "engine/raytracing/dispatch/DxrPathTracerDispatch.h"
#include "engine/raytracing/dispatch/DxrReflectionsDispatch.h"
#include "engine/raytracing/dispatch/DxrRestirDispatch.h"
#include "engine/raytracing/dispatch/DxrShadowsDispatch.h"
#include "engine/raytracing/dispatch/DxrSmokeDispatch.h"

#include "engine/rendering/core/RenderDebug.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
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
class MeshShaderGBufferRenderer;
class MeshShaderShadowRenderer;
class ScreenSpaceEffects;
class Shader;

struct RenderFrameDiagnostics
{
    std::uint32_t renderableObjectCount = 0;
    std::uint32_t gpuSceneInstanceCount = 0;
    std::uint32_t gpuSceneMeshAssetCount = 0;
    std::uint32_t gpuSceneMaterialCount = 0;
    std::uint32_t uniqueMeshCount = 0;
    std::uint32_t renderableMeshletCount = 0;
    std::uint32_t uniqueMeshletCount = 0;
    std::uint32_t meshletVertexReferenceCount = 0;
    std::uint32_t meshletTriangleCount = 0;
    std::uint32_t selectedRenderInstanceCount = 0;
    std::uint32_t previousWorldResolvedCount = 0;
    std::uint32_t previousWorldInitializedCount = 0;
    std::uint32_t gpuSceneUploadFrameCount = 0;
    std::uint32_t gpuSceneResizeEventCount = 0;
    std::uint32_t gpuSceneInstanceSrvIndex = 0xFFFFFFFFu;
    std::uint32_t gpuSceneMeshAssetSrvIndex = 0xFFFFFFFFu;
    std::uint32_t gpuSceneMaterialSrvIndex = 0xFFFFFFFFu;
    std::uint64_t gpuSceneInstanceBytes = 0;
    std::uint64_t gpuSceneMeshAssetBytes = 0;
    std::uint64_t gpuSceneMaterialBytes = 0;
    std::uint32_t primarySelectionInstanceId = 0xFFFFFFFFu;
    SceneObjectId primarySelectionEditorObjectId = kInvalidSceneObjectId;
    std::uint32_t primarySelectionMeshId = 0xFFFFFFFFu;
    std::uint32_t primarySelectionMaterialId = 0xFFFFFFFFu;
    std::uint32_t geometryDrawCount = 0;
    std::uint32_t shadowDrawCount = 0;
    std::uint32_t shadowCascadeCount = 0;
    std::uint32_t shadowMeshShaderDispatchCount = 0;
    std::uint32_t meshShaderDispatchCount = 0;
    double rendererCpuMs = 0.0;
    double gpuSceneBuildCpuMs = 0.0;
    double gpuSceneUploadCpuMs = 0.0;
    double lightingSyncCpuMs = 0.0;
    double shadowRecordCpuMs = 0.0;
    double rasterTargetSetupCpuMs = 0.0;
    double rasterRecordCpuMs = 0.0;
    double postProcessCpuMs = 0.0;
    double gizmoCpuMs = 0.0;
    double dxrScenePrepCpuMs = 0.0;
    double pathTracerFrameDataCpuMs = 0.0;
    bool instanceEditorIdMapValid = true;
    bool gpuSceneUploadValid = false;
    bool meshShadersSupported = false;
    bool meshShaderGBufferActive = false;
    bool pathTracingActive = false;
};

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
    void ResetPathTracerRestirDiagnosticState();

    DxrSettings& GetDxrSettings();
    const DxrSettings& GetDxrSettings() const;
    DxrPathTracerDispatch::SerOverride GetPathTracerSerOverride() const;
    void SetPathTracerSerOverride(DxrPathTracerDispatch::SerOverride value);
    bool IsPathTracerSerActive() const;

    const DxrDiagnostics& GetDxrDiagnostics() const;
    const RenderFrameDiagnostics& GetRenderFrameDiagnostics() const { return m_renderFrameDiagnostics; }
    const GpuScene& GetGpuScene() const { return m_gpuScene; }
    std::uint32_t FindRenderInstanceForObjectIndex(std::uint32_t objectIndex) const;

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
    // Clears everything keyed by project scene content while retaining scene-independent shaders,
    // pipelines, and reusable render targets so reopening does not incur a cold renderer ramp.
    void ResetProjectState();
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
    // Structural scene edits cannot be reconstructed from motion vectors. Reset temporal consumers
    // immediately before the next render so deleted/inserted content never survives in history.
    void NotifySceneContentChanged();
    // Must run before BeginFrame — constructing ScreenSpaceEffects uploads shaders via ExecuteImmediate.
    void PrepareGameViewGpuResources();
    void InvalidateGameViewMotionOnPlayStop();

private:
    [[noreturn]] void ThrowGpuResourcesUnavailable() const;
    void EnsureGpuResources() const;
    void ResetPartialGpuResources() const;
    void ApplyPendingSceneContentInvalidation();
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
        bool splitLightingMrt,
        RenderViewport renderViewport);
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
        bool usePostProcess,
        RenderViewport renderViewport);
    void SyncEffectiveMaterialMipBias() const;
    void EnsureGameViewScreenSpaceEffects();
    void SyncGameViewScreenSpaceSettings();
    void MaybeInvalidateStaleViewportTemporalState(RenderViewport viewport);
    void InvalidateViewportTemporalState(RenderViewport viewport);
    void SyncPtTemporalHistoryVersion();
    void ApplyPtSceneVersionInvalidation();
    void AdvancePreviousWorldTransforms();

    std::unique_ptr<CameraGizmoRenderer> m_cameraGizmos;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<ColliderGizmoRenderer> m_colliderGizmos;
    std::unique_ptr<LightGizmoRenderer> m_lightGizmos;
    std::unique_ptr<Shader> m_shadowDepthShader;
    std::unique_ptr<CascadedShadowMap> m_shadowMap;
    std::unique_ptr<EnvironmentMap> m_environmentMap;
    std::unique_ptr<ScreenSpaceEffects> m_screenSpaceEffects;
    std::unique_ptr<ScreenSpaceEffects> m_gameViewScreenSpaceEffects;
    // Keeps the startup PBR prewarm alive only until scene materials have adopted it on the first
    // geometry pass. It must not outlive that hand-off, so material shader invalidation continues
    // to rebuild with current renderer sampler settings.
    std::shared_ptr<Shader> m_preWarmedPbrShader;
    std::unique_ptr<MeshShaderGBufferRenderer> m_meshShaderGBufferRenderer;
    std::unique_ptr<MeshShaderShadowRenderer> m_meshShaderShadowRenderer;
    ScreenSpaceEffects* m_activeScreenSpaceEffects = nullptr;
    std::unique_ptr<DxrAccelerationStructures> m_dxrAccelerationStructures;
    std::unique_ptr<DxrSmokeDispatch> m_dxrSmokeDispatch;
    std::unique_ptr<DxrPrimaryDebugDispatch> m_dxrPrimaryDebugDispatch;
    std::unique_ptr<DxrPathTracerDispatch> m_dxrPathTracerDispatch;
    std::unique_ptr<DxrReflectionsDispatch> m_dxrReflectionsDispatch;
    std::unique_ptr<DxrShadowsDispatch> m_dxrShadowsDispatch;
    std::unique_ptr<DxrGiDispatch> m_dxrGiDispatch;
    std::unique_ptr<DxrRestirDispatch> m_dxrRestirDispatch;
    DxrSettings m_dxrSettings;
    DirectionalShadowSettings m_directionalShadowSettings;
    TextureFilterMode m_textureFilterMode = TextureFilterMode::Trilinear;
    std::uint32_t m_textureAnisotropy = 8;
    float m_textureMipBias = 0.0f;
    SceneLighting m_lighting;
    mutable GpuScene::PreviousWorldMap m_previousWorldByObjectId;
    mutable GpuScene::PreviousWorldMap m_gameViewPreviousWorldByObjectId;
    std::uint64_t m_sceneViewLastSubmissionFrame = 0;
    std::uint64_t m_gameViewLastSubmissionFrame = 0;
    GpuScene::PreviousWorldMap* m_activePreviousWorldByObjectId = nullptr;
    mutable GpuResourceState m_gpuResourceState = GpuResourceState::NotStarted;
    mutable std::string m_gpuResourcesInitError;
    nlohmann::json m_pendingRendererSettings;
    bool m_hasPendingRendererSettings = false;
    bool m_geometryMsaaReloadRequested = false;
    bool m_geometryMsaaReloadFailed = false;
    std::string m_geometryMsaaReloadError;
    bool m_sceneContentInvalidationPending = false;
    std::uint32_t m_consumedPtSceneVersion = 0;
    std::uint64_t m_ptEnvironmentFingerprint = 0;
    std::uint64_t m_ptSettingsFingerprint = 0;
    RenderFrameDiagnostics m_renderFrameDiagnostics{};
    GpuScene m_gpuScene;
};
