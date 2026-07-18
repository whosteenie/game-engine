#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/passes/GridRenderer.h"

#include "app/scene/rendering/SceneRenderer.h"

#include "app/scene/rendering/GpuSceneBuilder.h"

#include "app/project/SceneProjectIODetail.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/system/BackgroundWork.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/platform/tooling/ProjectLoadProgress.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"

#include "app/scene/editing/SceneEditor.h"
#include "engine/camera/Camera.h"
#include "engine/rhi/GfxContext.h"
#include "engine/components/LightComponent.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/rendering/passes/MeshShaderGBufferRenderer.h"
#include "engine/rendering/passes/MeshShaderShadowRenderer.h"
#include "engine/rendering/core/MotionVectorFrameState.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rendering/core/DxrSettings.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rendering/shaders/ShaderCache.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/raytracing/pipeline/DxrShaderCache.h"
#include "engine/raytracing/core/DxrTrace.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
void SceneRenderer::ThrowGpuResourcesUnavailable() const
{
    throw std::runtime_error(
        m_gpuResourcesInitError.empty()
            ? std::string("GPU resources are not initialized.")
            : m_gpuResourcesInitError);
}

namespace
{
    template<typename Fn>
    void RunGpuInitStep(const char* stepName, const char* benchmarkPhase, float progress, Fn&& fn)
    {
        ProjectLoadProgress::Report(
            std::string("Initializing GPU: ") + stepName + "...",
            progress);
        ProjectLoadBenchmark::ScopedPhase benchmarkScope(benchmarkPhase);
        SceneRenderTrace::Scope initScope(stepName);
        try
        {
            fn();
            initScope.Success();
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                std::string("GPU init step '") + stepName + "' failed: " + SafeExceptionMessage(exception));
        }
        catch (...)
        {
            throw std::runtime_error(std::string("GPU init step '") + stepName + "' failed: unknown error");
        }
    }
}

void SceneRenderer::ResetPartialGpuResources() const
{
    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    self->m_cameraGizmos.reset();
    self->m_grid.reset();
    self->m_colliderGizmos.reset();
    self->m_lightGizmos.reset();
    self->m_shadowMap.reset();
    self->m_environmentMap.reset();
    self->m_screenSpaceEffects.reset();
    self->m_gameViewScreenSpaceEffects.reset();
    self->m_dxrAccelerationStructures.reset();
    self->m_dxrSmokeDispatch.reset();
    self->m_dxrPrimaryDebugDispatch.reset();
    self->m_dxrReflectionsDispatch.reset();
    self->m_dxrShadowsDispatch.reset();
    self->m_dxrGiDispatch.reset();
    self->m_dxrRestirDispatch.reset();
    self->m_shadowDepthShader.reset();
    self->m_gpuResourceState = GpuResourceState::NotStarted;
}

void SceneRenderer::ResetGpuResourcesIfInitFailed() const
{
    if (IsGpuResourcesReady() || !HasGpuResourcesInitFailed())
    {
        return;
    }

    ResetPartialGpuResources();
    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    self->m_gpuResourceState = GpuResourceState::NotStarted;
    self->m_gpuResourcesInitError.clear();
}

void SceneRenderer::ResetProjectState()
{
    // These structures contain direct Mesh* and Material* keys owned by the outgoing scene.
    m_dxrAccelerationStructures.reset();
    // Retain scene-independent RTPSOs and shader binding tables, but discard every project-sized
    // output, descriptor, denoiser pool, reservoir, and temporal-history resource.
    if (m_dxrSmokeDispatch != nullptr) m_dxrSmokeDispatch->ResetProjectResources();
    if (m_dxrPrimaryDebugDispatch != nullptr) m_dxrPrimaryDebugDispatch->ResetProjectResources();
    if (m_dxrPathTracerDispatch != nullptr) m_dxrPathTracerDispatch->ResetProjectResources();
    if (m_dxrReflectionsDispatch != nullptr) m_dxrReflectionsDispatch->ResetProjectResources();
    if (m_dxrShadowsDispatch != nullptr) m_dxrShadowsDispatch->ResetProjectResources();
    if (m_dxrGiDispatch != nullptr) m_dxrGiDispatch->ResetProjectResources();
    if (m_dxrRestirDispatch != nullptr) m_dxrRestirDispatch->ResetProjectResources();
    m_gpuScene.Clear();
    // Keep table capacity and descriptors allocated. The next Build replaces every CPU record and
    // UploadGpuTables overwrites the GPU rings before use, avoiding a needless allocation ramp.

    m_previousWorldByObjectId.clear();
    m_gameViewPreviousWorldByObjectId.clear();
    m_activePreviousWorldByObjectId = nullptr;
    m_activeScreenSpaceEffects = nullptr;
    m_sceneViewLastSubmissionFrame = 0;
    m_gameViewLastSubmissionFrame = 0;

    // Preserve the allocated effects and their shader pipelines, but invalidate every history that
    // can encode the outgoing project's geometry, radiance, motion, DLSS/RR, or accumulation.
    if (m_screenSpaceEffects != nullptr)
    {
        m_screenSpaceEffects->InvalidateAllTemporalState();
        m_screenSpaceEffects->ResetPathTracerTemporalDiagnostics();
    }
    if (m_gameViewScreenSpaceEffects != nullptr)
    {
        m_gameViewScreenSpaceEffects->InvalidateAllTemporalState();
        m_gameViewScreenSpaceEffects->ResetPathTracerTemporalDiagnostics();
    }
    m_lighting.ClearLights();
    m_pendingRendererSettings = nlohmann::json{};
    m_hasPendingRendererSettings = false;
    m_geometryMsaaReloadRequested = false;
    m_geometryMsaaReloadFailed = false;
    m_geometryMsaaReloadError.clear();
    m_sceneContentInvalidationPending = true;
    m_consumedPtSceneVersion = 0;
    m_ptEnvironmentFingerprint = 0;
    m_ptSettingsFingerprint = 0;
    m_renderFrameDiagnostics = {};
}

void SceneRenderer::PrepareGpuResourcesForGeometryMsaa(const int msaaSampleCount) const
{
    GfxContext::Get().SetActiveMsaaSampleCount(msaaSampleCount);
    const int activeMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();

    if (IsGpuResourcesReady())
    {
        const bool geometryMsaaMatches =
            m_screenSpaceEffects != nullptr
            && m_screenSpaceEffects->GetMsaaSampleCount() == activeMsaaSampleCount
            && !m_screenSpaceEffects->IsMsaaPendingReload();
        if (geometryMsaaMatches)
        {
            return;
        }

        RenderingPipelineCache::InvalidateAll();
        ResetPartialGpuResources();
        SceneRenderer* self = const_cast<SceneRenderer*>(this);
        self->m_gpuResourceState = GpuResourceState::NotStarted;
        self->m_gpuResourcesInitError.clear();
    }
    else
    {
        ResetGpuResourcesIfInitFailed();
        if (activeMsaaSampleCount > 1)
        {
            RenderingPipelineCache::InvalidateAll();
        }
    }

    EnsureGpuResources();

    if (IsGpuResourcesReady() && m_screenSpaceEffects != nullptr
        && m_screenSpaceEffects->GetMsaaSampleCount() != activeMsaaSampleCount)
    {
        const_cast<ScreenSpaceEffects*>(m_screenSpaceEffects.get())->SetMsaaSampleCount(activeMsaaSampleCount);
    }
}

void SceneRenderer::EnsureGpuResources() const
{
    if (m_gpuResourceState != GpuResourceState::NotStarted)
    {
        return;
    }

    if (!GfxContext::Get().IsInitialized())
    {
        m_gpuResourceState = GpuResourceState::Failed;
        m_gpuResourcesInitError = "GfxContext is not initialized";
        EngineLog::Error("scene", "GPU init failed: " + m_gpuResourcesInitError);
        return;
    }

    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    self->m_gpuResourceState = GpuResourceState::InProgress;
    ProjectLoadBenchmark::ScopedPhase gpuInitializationPhase("renderer.gpu_resource_initialize");
    SceneRenderTrace::Scope gpuInitScope("EnsureGpuResources");
    try
    {
        RunGpuInitStep("camera gizmos", "renderer.gpu_init.camera_gizmos", ProjectLoadProgress::GpuInitialization(0.0f / 8.0f), [&]() { self->m_cameraGizmos = std::make_unique<CameraGizmoRenderer>(); });
        RunGpuInitStep("grid", "renderer.gpu_init.grid", ProjectLoadProgress::GpuInitialization(1.0f / 8.0f), [&]() { self->m_grid = std::make_unique<GridRenderer>(); });
        RunGpuInitStep("collider gizmos", "renderer.gpu_init.collider_gizmos", ProjectLoadProgress::GpuInitialization(2.0f / 8.0f), [&]() { self->m_colliderGizmos = std::make_unique<ColliderGizmoRenderer>(); });
        RunGpuInitStep("light gizmos", "renderer.gpu_init.light_gizmos", ProjectLoadProgress::GpuInitialization(3.0f / 8.0f), [&]() { self->m_lightGizmos = std::make_unique<LightGizmoRenderer>(); });
        RunGpuInitStep("shadow map", "renderer.gpu_init.shadow_map", ProjectLoadProgress::GpuInitialization(4.0f / 8.0f), [&]() { self->m_shadowMap = std::make_unique<CascadedShadowMap>(); });
        RunGpuInitStep("environment map", "renderer.gpu_init.environment_map", ProjectLoadProgress::GpuInitialization(5.0f / 8.0f), [&]() { self->m_environmentMap = std::make_unique<EnvironmentMap>(); });
        RunGpuInitStep("screen-space effects", "renderer.gpu_init.screen_space_effects", ProjectLoadProgress::GpuInitialization(6.0f / 8.0f), [&]() {
            ProjectLoadBenchmark::ScopedPhase screenSpaceStagePrewarm(
                "renderer.gpu_init.screen_space_shader_stage_prewarm");
            ScreenSpaceEffects::PrewarmShaderStages();
            self->m_screenSpaceEffects = std::make_unique<ScreenSpaceEffects>();
            const int geometryMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();
            if (geometryMsaaSampleCount > 1)
            {
                self->m_screenSpaceEffects->SetMsaaSampleCount(geometryMsaaSampleCount);
            }
        });
        RunGpuInitStep("shadow depth shader", "renderer.gpu_init.shadow_depth_shader", ProjectLoadProgress::GpuInitialization(7.0f / 8.0f), [&]() {
            self->m_shadowDepthShader = std::make_unique<Shader>(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
        });
        self->m_gpuResourceState = GpuResourceState::Ready;
        GfxContext::Get().SetMaterialTextureFilterMode(self->m_textureFilterMode);
        GfxContext::Get().SetMaterialTextureAnisotropy(self->m_textureAnisotropy);
        self->SyncEffectiveMaterialMipBias();
        gpuInitScope.Success();
    }
    catch (const std::exception& exception)
    {
        ResetPartialGpuResources();
        self->m_gpuResourceState = GpuResourceState::Failed;
        self->m_gpuResourcesInitError = SafeExceptionMessage(exception);
        EngineLog::Error("scene", "GPU init failed: " + self->m_gpuResourcesInitError);
    }
    catch (...)
    {
        ResetPartialGpuResources();
        self->m_gpuResourceState = GpuResourceState::Failed;
        self->m_gpuResourcesInitError = "unknown GPU initialization error";
        EngineLog::Error("scene", self->m_gpuResourcesInitError);
    }
}

void SceneRenderer::PrepareFrameGpuResources()
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady() || GfxContext::Get().IsFrameRecording())
    {
        return;
    }

    if (m_environmentMap != nullptr)
    {
        ProjectLoadProgress::Report(
            "Loading / syncing HDR environment...",
            ProjectLoadProgress::kEnvironmentSync);
        SceneRenderTrace::Scope envScope("SyncGpuResources");
        try
        {
            ProjectLoadBenchmark::ScopedPhase environmentSyncPhase("renderer.environment_sync");
            m_environmentMap->SyncGpuResources();
            envScope.Success();
        }
        catch (const std::exception& exception)
        {
            EngineLog::Error(
                "scene",
                std::string("SyncGpuResources failed: ") + SafeExceptionMessage(exception));
        }
    }

    if (m_dxrSettings.IsEnabled() && GfxContext::Get().IsRaytracingSupported())
    {
        ProjectLoadProgress::Report(
            "Warming ray tracing pipelines...",
            ProjectLoadProgress::kDxrWarmupStart);
        {
            ProjectLoadBenchmark::ScopedPhase dxrWarmupPhase("renderer.dxr_pipeline_warmup");
            WarmUpDxrPipelineIfNeeded();
        }
        ProjectLoadProgress::Report(
            "Ray tracing pipelines ready.",
            ProjectLoadProgress::kDxrWarmupEnd);
    }
    else
    {
        ProjectLoadProgress::Report(
            "Preparing first scene frame...",
            ProjectLoadProgress::kFirstSceneFrameStart);
    }
}

