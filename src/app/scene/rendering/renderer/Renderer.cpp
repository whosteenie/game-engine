#include "app/scene/rendering/SceneRenderer.h"
#include "app/scene/rendering/GpuSceneBuilder.h"
#include "app/scene/editing/SceneEditor.h"
#include "app/project/SceneProjectIODetail.h"
#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/platform/tooling/ProjectLoadProgress.h"
#include "engine/rhi/GfxContext.h"
#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/rendering/core/DxrSettings.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/rendering/passes/GridRenderer.h"
#include "engine/rendering/passes/MeshShaderGBufferRenderer.h"
#include "engine/rendering/passes/MeshShaderShadowRenderer.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/raytracing/core/DxrTrace.h"

#include <ImGuizmo.h>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
    class ScopedGameViewEffects final
    {
    public:
        ScopedGameViewEffects(
            std::unique_ptr<ScreenSpaceEffects>& sceneViewEffects,
            std::unique_ptr<ScreenSpaceEffects>& gameViewEffects,
            const bool activateGameView)
            : m_sceneViewEffects(sceneViewEffects),
              m_gameViewEffects(gameViewEffects),
              m_active(activateGameView)
        {
            if (m_active)
            {
                std::swap(m_sceneViewEffects, m_gameViewEffects);
            }
        }

        ~ScopedGameViewEffects()
        {
            if (m_active)
            {
                std::swap(m_sceneViewEffects, m_gameViewEffects);
            }
        }

        ScopedGameViewEffects(const ScopedGameViewEffects&) = delete;
        ScopedGameViewEffects& operator=(const ScopedGameViewEffects&) = delete;

    private:
        std::unique_ptr<ScreenSpaceEffects>& m_sceneViewEffects;
        std::unique_ptr<ScreenSpaceEffects>& m_gameViewEffects;
        bool m_active = false;
    };

    class ScopedDxrDisable final
    {
    public:
        ScopedDxrDisable(DxrSettings& settings, const bool disable)
            : m_settings(settings), m_wasEnabled(settings.IsEnabled()), m_active(disable)
        {
            if (m_active)
            {
                m_settings.SetEnabled(false);
            }
        }

        ~ScopedDxrDisable()
        {
            if (m_active)
            {
                m_settings.SetEnabled(m_wasEnabled);
            }
        }

        ScopedDxrDisable(const ScopedDxrDisable&) = delete;
        ScopedDxrDisable& operator=(const ScopedDxrDisable&) = delete;

    private:
        DxrSettings& m_settings;
        bool m_wasEnabled = false;
        bool m_active = false;
    };

    class ScopedRenderDebugOverride final
    {
    public:
        ScopedRenderDebugOverride(ScreenSpaceEffects* effects, const RenderDebugMode overrideMode)
            : m_effects(effects),
              m_originalMode(effects != nullptr ? effects->GetDebugMode() : RenderDebugMode::None)
        {
            if (m_effects != nullptr && m_originalMode != overrideMode)
            {
                m_effects->SetDebugMode(overrideMode);
                m_active = true;
            }
        }

        ~ScopedRenderDebugOverride()
        {
            if (m_active)
            {
                m_effects->SetDebugMode(m_originalMode);
            }
        }

        ScopedRenderDebugOverride(const ScopedRenderDebugOverride&) = delete;
        ScopedRenderDebugOverride& operator=(const ScopedRenderDebugOverride&) = delete;

    private:
        ScreenSpaceEffects* m_effects = nullptr;
        RenderDebugMode m_originalMode = RenderDebugMode::None;
        bool m_active = false;
    };

}

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::EnsureGameViewScreenSpaceEffects()
{
    if (m_gameViewScreenSpaceEffects == nullptr)
    {
        // Keep the game camera's temporal/post state entirely separate from the editor camera.
        // The Streamline viewport id must match that separation as well.
        m_gameViewScreenSpaceEffects = std::make_unique<ScreenSpaceEffects>(1);
        if (m_screenSpaceEffects != nullptr)
        {
            m_gameViewScreenSpaceEffects->CopySettingsFrom(*m_screenSpaceEffects);
        }
        const int msaa = GfxContext::Get().GetActiveMsaaSampleCount();
        if (msaa > 1)
        {
            m_gameViewScreenSpaceEffects->SetMsaaSampleCount(msaa);
        }
    }
}

void SceneRenderer::SyncGameViewScreenSpaceSettings()
{
    if (m_gameViewScreenSpaceEffects != nullptr && m_screenSpaceEffects != nullptr)
    {
        m_gameViewScreenSpaceEffects->CopySettingsFrom(*m_screenSpaceEffects);
    }
}

void SceneRenderer::PrepareGameViewGpuResources()
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady())
    {
        return;
    }
    EnsureGameViewScreenSpaceEffects();
    SyncGameViewScreenSpaceSettings();
}

void SceneRenderer::NotifySceneContentChanged()
{
    m_sceneContentInvalidationPending = true;
}

void SceneRenderer::ApplyPendingSceneContentInvalidation()
{
    if (!m_sceneContentInvalidationPending)
    {
        return;
    }

    // Keep the reset at the render boundary: edits can happen while UI code is running, while the
    // effect histories are only safe to invalidate before their next GPU use. This covers TAA,
    // DLSS/RR, temporal bloom, screen-space histories, and reference PT accumulation.
    if (m_screenSpaceEffects != nullptr)
    {
        m_screenSpaceEffects->InvalidateAllTemporalState();
    }
    if (m_gameViewScreenSpaceEffects != nullptr)
    {
        m_gameViewScreenSpaceEffects->InvalidateAllTemporalState();
    }

    // Real-time PT owns ReSTIR and its own accumulation independently of ScreenSpaceEffects.
    if (m_dxrPathTracerDispatch != nullptr)
    {
        for (const RenderViewport viewport : {RenderViewport::SceneView, RenderViewport::GameView})
        {
            const std::uint32_t viewportId = RenderViewportHistoryId(viewport);
            m_dxrPathTracerDispatch->ResetAccumulation(viewportId);
            m_dxrPathTracerDispatch->InvalidateRestirHistory(viewportId);
        }
    }

    m_sceneContentInvalidationPending = false;
}

void SceneRenderer::InvalidateGameViewMotionOnPlayStop()
{
    m_gameViewPreviousWorldByObjectId.clear();
    if (m_gameViewScreenSpaceEffects != nullptr)
    {
        m_gameViewScreenSpaceEffects->InvalidateMotionHistory();
    }
}

void SceneRenderer::SetRenderDebugMode(const RenderDebugMode mode)
{
    EnsureGpuResources();
    if (m_screenSpaceEffects != nullptr)
    {
        m_screenSpaceEffects->SetDebugMode(mode);
    }
}

RenderDebugMode SceneRenderer::GetRenderDebugMode() const
{
    return m_screenSpaceEffects != nullptr
        ? m_screenSpaceEffects->GetDebugMode()
        : RenderDebugMode::None;
}

void SceneRenderer::ResetPathTracerRestirDiagnosticState()
{
    if (m_screenSpaceEffects != nullptr)
    {
        m_screenSpaceEffects->ResetPathTracerAccumulation();
        m_screenSpaceEffects->ResetPathTracerTemporalDiagnostics();
    }
    if (m_dxrPathTracerDispatch != nullptr)
    {
        const std::uint32_t sceneViewportId = RenderViewportHistoryId(RenderViewport::SceneView);
        m_dxrPathTracerDispatch->ResetAccumulation(sceneViewportId);
        m_dxrPathTracerDispatch->InvalidateRestirHistory(sceneViewportId);
    }
}

void SceneRenderer::MergePendingRendererSettings(const nlohmann::json& delta)
{
    SceneProjectIODetail::MergeRendererSettings(m_pendingRendererSettings, delta);
    m_hasPendingRendererSettings = true;
}

nlohmann::json SceneRenderer::TakePendingRendererSettings()
{
    nlohmann::json pending = std::move(m_pendingRendererSettings);
    m_pendingRendererSettings = nlohmann::json::object();
    m_hasPendingRendererSettings = false;
    return pending;
}

void SceneRenderer::Render(
    const Scene& scene,
    const Camera& camera,
    int viewportWidth,
    int viewportHeight,
    std::uintptr_t targetFramebuffer,
    const SceneRenderOptions& options,
    const RenderViewport renderViewport)
{
    SceneRenderTrace::Scope renderScope("SceneRenderer::Render");
    const auto renderCpuStart = std::chrono::steady_clock::now();
    m_renderFrameDiagnostics.rendererCpuMs = 0.0;
    m_renderFrameDiagnostics.gpuSceneBuildCpuMs = 0.0;
    m_renderFrameDiagnostics.gpuSceneUploadCpuMs = 0.0;
    m_renderFrameDiagnostics.lightingSyncCpuMs = 0.0;
    m_renderFrameDiagnostics.shadowRecordCpuMs = 0.0;
    m_renderFrameDiagnostics.rasterTargetSetupCpuMs = 0.0;
    m_renderFrameDiagnostics.rasterRecordCpuMs = 0.0;
    m_renderFrameDiagnostics.postProcessCpuMs = 0.0;
    m_renderFrameDiagnostics.gizmoCpuMs = 0.0;
    m_renderFrameDiagnostics.dxrScenePrepCpuMs = 0.0;
    m_renderFrameDiagnostics.pathTracerFrameDataCpuMs = 0.0;

    ProjectLoadProgress::Report(
        "Recording first scene frame...",
        ProjectLoadProgress::kFirstSceneFrameStart);
    EnsureGpuResources();
    if (!IsGpuResourcesReady())
    {
        m_renderFrameDiagnostics.rendererCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - renderCpuStart)
                .count();
        renderScope.Success();
        return;
    }

    const bool isGameView = renderViewport == RenderViewport::GameView;
    if (isGameView)
    {
        EnsureGameViewScreenSpaceEffects();
        SyncGameViewScreenSpaceSettings();
    }

    ApplyPendingSceneContentInvalidation();

    // Most renderer code intentionally addresses m_screenSpaceEffects directly. Swap the
    // active owner only for this sequential render call so all existing passes (including
    // resize, history, and post processing) operate on the correct viewport's resources.
    ScopedGameViewEffects activeEffects(
        m_screenSpaceEffects,
        m_gameViewScreenSpaceEffects,
        isGameView);

    // Game View is always runtime presentation: renderer diagnostics belong exclusively to
    // Scene View. A selected Scene diagnostic is explicit and therefore takes precedence over
    // the Scene View's normal Lit/Unlit presentation choice.
    const SceneViewShadingMode shadingMode = isGameView
        ? SceneViewShadingMode::FullRuntime
        : options.shadingMode;
    const RenderDebugMode requestedDebugMode = m_screenSpaceEffects->GetDebugMode();
    const bool sceneDiagnosticActive = !isGameView && requestedDebugMode != RenderDebugMode::None;
    const RenderDebugMode effectiveDebugMode = isGameView
        ? RenderDebugMode::None
        : sceneDiagnosticActive
            ? requestedDebugMode
            : shadingMode == SceneViewShadingMode::Unlit
                ? RenderDebugMode::GBufferAlbedo
                : RenderDebugMode::None;
    const bool suppressDxrForSceneView = !isGameView
        && !sceneDiagnosticActive
        && shadingMode != SceneViewShadingMode::FullRuntime;
    ScopedDxrDisable scopedDxrDisable(m_dxrSettings, suppressDxrForSceneView);
    ScopedRenderDebugOverride scopedDebugMode(m_screenSpaceEffects.get(), effectiveDebugMode);

    ProjectLoadProgress::Report(
        "Building scene GPU tables...",
        ProjectLoadProgress::kSceneGpuTableBuildStart);
    m_activePreviousWorldByObjectId = isGameView
        ? &m_gameViewPreviousWorldByObjectId
        : &m_previousWorldByObjectId;
    m_activeScreenSpaceEffects = m_screenSpaceEffects.get();

    const auto gpuSceneBuildStart = std::chrono::steady_clock::now();
    {
        ProjectLoadBenchmark::ScopedPhase gpuSceneBuildPhase("renderer.first_scene.gpu_scene_build");
        GpuSceneBuilder::Build(m_gpuScene, scene, *m_activePreviousWorldByObjectId);
    }
    m_renderFrameDiagnostics.gpuSceneBuildCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - gpuSceneBuildStart)
            .count();

    ProjectLoadProgress::Report("Uploading scene GPU tables...", ProjectLoadProgress::kSceneUpload);
    const auto gpuSceneUploadStart = std::chrono::steady_clock::now();
    {
        ProjectLoadBenchmark::ScopedPhase gpuSceneUploadPhase("renderer.first_scene.gpu_scene_upload");
        m_gpuScene.UploadGpuTables(GfxContext::Get().GetCommandList());
    }
    m_renderFrameDiagnostics.gpuSceneUploadCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - gpuSceneUploadStart)
            .count();

    Framebuffer* target = nullptr;
    if (targetFramebuffer != 0)
    {
        target = reinterpret_cast<Framebuffer*>(targetFramebuffer);
    }

    const bool usePostProcess = m_screenSpaceEffects->IsEnabled();
    // Lit and Unlit are inspection views, not temporal presentation modes.  They deliberately
    // bypass the runtime's ray tracing path, so retain a stable projection rather than exposing
    // the raster jitter that normally feeds TAA/DLSS/RR.
    const bool freezeTemporalJitter = ImGuizmo::IsUsing() || ImGuizmo::IsUsingViewManipulate()
        || m_screenSpaceEffects->GetFreezeTemporalJitterDiagnostic()
        || (!isGameView && !sceneDiagnosticActive && shadingMode != SceneViewShadingMode::FullRuntime);

    SceneRenderTrace::Step(
        std::string("render setup postProcess=") + (usePostProcess ? "1" : "0")
        + " shadowPass=" + (options.enableShadowPass ? "1" : "0"));

    if (target != nullptr)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(target);
    }

    {
        SceneRenderTrace::Scope lightingScope("SyncLighting");
        ProjectLoadProgress::Report("Syncing scene lighting...", ProjectLoadProgress::kSceneLighting);
        const auto lightingSyncStart = std::chrono::steady_clock::now();
        {
            ProjectLoadBenchmark::ScopedPhase lightingPhase("renderer.first_scene.lighting_sync");
            SyncLighting(scene);
        }
        m_renderFrameDiagnostics.lightingSyncCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - lightingSyncStart)
                .count();
        lightingScope.Success();
    }

    if (options.enableShadowPass)
    {
        SceneRenderTrace::Scope shadowScope("RenderShadowPass");
        ProjectLoadProgress::Report("Rendering shadow maps...", ProjectLoadProgress::kSceneShadows);
        const auto shadowRecordStart = std::chrono::steady_clock::now();
        const GfxContext::GpuTimerScope gpuScopeShadowMaps("Shadow maps");
        {
            ProjectLoadBenchmark::ScopedPhase shadowPhase("renderer.first_scene.shadow_record");
            RenderShadowPass(scene, camera);
        }
        m_shadowMap->EndFrame();
        m_renderFrameDiagnostics.shadowRecordCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - shadowRecordStart)
                .count();
        shadowScope.Success();
    }

    RenderDebugMode materialDebugMode = RenderDebugMode::None;
    const RenderDebugMode activeDebugMode = m_screenSpaceEffects->GetDebugMode();
    if (IsPbrMaterialDebugMode(activeDebugMode))
    {
        materialDebugMode = activeDebugMode;
    }

    bool splitLightingMrt = false;
    ProjectLoadProgress::Report("Rasterizing scene...", ProjectLoadProgress::kSceneRaster);
    const auto rasterTargetSetupStart = std::chrono::steady_clock::now();
    {
        ProjectLoadBenchmark::ScopedPhase rasterTargetPhase("renderer.first_scene.raster_target_setup");
        PrepareSceneRasterTarget(
            scene,
            camera,
            viewportWidth,
            viewportHeight,
            target,
            usePostProcess,
            freezeTemporalJitter,
            splitLightingMrt);
    }
    m_renderFrameDiagnostics.rasterTargetSetupCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rasterTargetSetupStart)
            .count();

    {
        ProjectLoadBenchmark::ScopedPhase geometryPhase("renderer.first_scene.geometry_record");
        RenderGeometryPass(
            scene,
            camera,
            usePostProcess,
            target,
            splitLightingMrt,
            materialDebugMode,
            activeDebugMode);
    }
    // Material instances now hold their own shared cache references. Drop the temporary startup
    // reference so normal material invalidation retains its existing shader-rebuild semantics.
    m_preWarmedPbrShader.reset();

    if (usePostProcess)
    {
        ProjectLoadProgress::Report(
            "Running post-process and ray tracing...",
            ProjectLoadProgress::kScenePostProcess);
        const auto postProcessStart = std::chrono::steady_clock::now();
        {
            ProjectLoadBenchmark::ScopedPhase postProcessPhase("renderer.first_scene.post_process");
            RenderPostProcessPass(
                scene,
                camera,
                viewportWidth,
                viewportHeight,
                target,
                options,
                freezeTemporalJitter,
                splitLightingMrt,
                renderViewport);
        }
        m_renderFrameDiagnostics.postProcessCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - postProcessStart)
                .count();
    }
    else if (options.showGrid && scene.GetShowGrid())
    {
        RecordDxrPass(
            scene, camera, viewportWidth, viewportHeight, 0, false, renderViewport);
        m_grid->Draw(camera, false);
    }
    else
    {
        RecordDxrPass(
            scene, camera, viewportWidth, viewportHeight, 0, false, renderViewport);
    }

    if (!usePostProcess && options.showEditorOverlay)
    {
        if (const SceneEditor* editor = scene.TryGetSceneEditor())
        {
            editor->RenderSelectionOverlay(scene, camera, false);
        }
    }

    const auto gizmoStart = std::chrono::steady_clock::now();
    {
        ProjectLoadBenchmark::ScopedPhase gizmoPhase("renderer.first_scene.gizmo_record");
        RenderGizmoPass(scene, camera, target, options, usePostProcess);
    }
    m_renderFrameDiagnostics.gizmoCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - gizmoStart)
            .count();

    AdvancePreviousWorldTransforms();

    m_renderFrameDiagnostics.rendererCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - renderCpuStart)
            .count();

    renderScope.Success();
    SceneRenderTrace::CompleteFirstFrame();
}

void SceneRenderer::AdvancePreviousWorldTransforms()
{
    if (m_activePreviousWorldByObjectId == nullptr)
    {
        return;
    }
    GpuScene::PreviousWorldMap& previous = *m_activePreviousWorldByObjectId;
    previous.clear();
    previous.reserve(m_gpuScene.GetInstances().size());
    for (const GpuSceneInstanceRecord& instance : m_gpuScene.GetInstances())
    {
        if (instance.editorObjectId != kInvalidSceneObjectId)
        {
            previous[instance.editorObjectId] = instance.world;
        }
    }
}

const SceneLighting& SceneRenderer::GetLighting() const
{
    return m_lighting;
}

SceneLighting& SceneRenderer::GetLighting()
{
    return m_lighting;
}

IBL& SceneRenderer::GetIBL()
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return m_environmentMap->GetIBL();
}

const IBL& SceneRenderer::GetIBL() const
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return m_environmentMap->GetIBL();
}

EnvironmentMap& SceneRenderer::GetEnvironmentMap()
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_environmentMap;
}

const EnvironmentMap& SceneRenderer::GetEnvironmentMap() const
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_environmentMap;
}

ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects()
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_screenSpaceEffects == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_screenSpaceEffects;
}

const ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects() const
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_screenSpaceEffects == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_screenSpaceEffects;
}

DxrSettings& SceneRenderer::GetDxrSettings()
{
    return m_dxrSettings;
}

const DxrSettings& SceneRenderer::GetDxrSettings() const
{
    return m_dxrSettings;
}

DxrPathTracerDispatch::SerOverride SceneRenderer::GetPathTracerSerOverride() const
{
    return m_dxrPathTracerDispatch != nullptr
        ? m_dxrPathTracerDispatch->GetSerOverride()
        : DxrPathTracerDispatch::SerOverride::Automatic;
}

void SceneRenderer::SetPathTracerSerOverride(const DxrPathTracerDispatch::SerOverride value)
{
    if (m_dxrPathTracerDispatch == nullptr)
    {
        m_dxrPathTracerDispatch = std::make_unique<DxrPathTracerDispatch>();
    }
    m_dxrPathTracerDispatch->SetSerOverride(value);
}

bool SceneRenderer::IsPathTracerSerActive() const
{
    return m_dxrPathTracerDispatch != nullptr
        && m_dxrPathTracerDispatch->IsSerActive(
            RenderViewportHistoryId(RenderViewport::SceneView));
}

const DxrDiagnostics& SceneRenderer::GetDxrDiagnostics() const
{
    static const DxrDiagnostics kEmptyDiagnostics{};
    if (m_dxrAccelerationStructures == nullptr)
    {
        return kEmptyDiagnostics;
    }

    return m_dxrAccelerationStructures->GetDiagnostics();
}

const DirectionalShadowSettings& SceneRenderer::GetDirectionalShadowSettings() const
{
    return m_directionalShadowSettings;
}

DirectionalShadowSettings& SceneRenderer::GetDirectionalShadowSettings()
{
    return m_directionalShadowSettings;
}

const CascadedShadowMap& SceneRenderer::GetShadowMap() const
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_shadowMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_shadowMap;
}

void SceneRenderer::SetTextureFilterMode(const TextureFilterMode mode)
{
    m_textureFilterMode = mode;
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().SetMaterialTextureFilterMode(mode);
    }
}

void SceneRenderer::SetTextureAnisotropy(const std::uint32_t anisotropy)
{
    m_textureAnisotropy = std::clamp(anisotropy, 1u, 16u);
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().SetMaterialTextureAnisotropy(m_textureAnisotropy);
    }
}

void SceneRenderer::SyncEffectiveMaterialMipBias() const
{
    if (!GfxContext::Get().IsInitialized())
    {
        return;
    }

    float effectiveBias = m_textureMipBias;
    if (m_screenSpaceEffects != nullptr)
    {
        effectiveBias += m_screenSpaceEffects->GetAutoMaterialMipBias();
    }

    GfxContext::Get().SetMaterialTextureMipBias(effectiveBias);
}

void SceneRenderer::SetTextureMipBias(const float mipBias)
{
    m_textureMipBias = std::clamp(mipBias, -4.0f, 4.0f);
    SyncEffectiveMaterialMipBias();
}

bool SceneRenderer::ApplyGeometryMsaaReload(
    Scene& scene,
    const int viewportWidth,
    const int viewportHeight,
    std::string* outError)
{
    // This is the single point of truth for the reload result; the editor reads it back for display.
    m_geometryMsaaReloadRequested = false;
    m_geometryMsaaReloadFailed = false;
    m_geometryMsaaReloadError.clear();

    auto reportFailure = [&](const std::string& message) {
        m_geometryMsaaReloadFailed = true;
        m_geometryMsaaReloadError = message;
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    EnsureGpuResources();
    if (!IsGpuResourcesReady() || m_screenSpaceEffects == nullptr)
    {
        return reportFailure("GPU resources are not initialized.");
    }

    if (!m_screenSpaceEffects->IsMsaaPendingReload())
    {
        return true;
    }

    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return reportFailure("Viewport size is invalid.");
    }

    const int requestedMsaaSampleCount = m_screenSpaceEffects->GetMsaaSampleCount();

    try
    {
        GfxContext::Get().CancelFrame();
        GfxContext::Get().WaitForGpuIdle();
        GfxContext::Get().SetBoundOutputFramebuffer(nullptr);
        // Drop the previous frame's command-list object references before we start releasing
        // pipelines/framebuffers below, or the D3D12 debug layer faults on the first PSO release.
        GfxContext::Get().ResetCommandListForTeardown();

        GfxContext::Get().SetActiveMsaaSampleCount(requestedMsaaSampleCount);
        RenderingPipelineCache::InvalidateAll();
        scene.InvalidateAllMaterialCachedShaders();
        m_environmentMap->ReloadSkyboxRenderer();
        m_screenSpaceEffects->ReloadGeometryMsaaTargets(viewportWidth, viewportHeight);

        m_gpuResourcesInitError.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        const std::string message = SafeExceptionMessage(exception);
        EngineLog::Error("scene", "MSAA reload failed: " + message);
        return reportFailure(message);
    }
    catch (...)
    {
        EngineLog::Error("scene", "MSAA reload failed: unknown error");
        return reportFailure("unknown MSAA reload error");
    }
}
