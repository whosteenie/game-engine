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

void SceneRenderer::ThrowGpuResourcesUnavailable() const
{
    throw std::runtime_error(
        m_gpuResourcesInitError.empty()
            ? std::string("GPU resources are not initialized.")
            : m_gpuResourcesInitError);
}

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

void SceneRenderer::SyncLighting(const Scene& scene)
{
    m_lighting.ClearLights();
    int shadowLightIndex = -1;

    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.HasLight())
        {
            continue;
        }

        if (m_lighting.GetLightCount() >= static_cast<std::size_t>(SceneLighting::MaxLights))
        {
            break;
        }

        const int lightSlot = static_cast<int>(m_lighting.GetLightCount());
        m_lighting.AddLight(BuildLightFromSceneObject(object, scene.GetWorldMatrix(static_cast<int>(objectIndex))));

        if (shadowLightIndex < 0 && object.GetLight().castsShadow)
        {
            shadowLightIndex = lightSlot;
        }
    }

    m_lighting.SetShadowLightIndex(shadowLightIndex);
}

glm::vec3 SceneRenderer::GetSunDirection() const
{
    const int shadowLightIndex = m_lighting.GetShadowLightIndex();
    const std::vector<Light>& lights = m_lighting.GetLights();
    if (shadowLightIndex >= 0 &&
        static_cast<std::size_t>(shadowLightIndex) < m_lighting.GetLightCount())
    {
        return lights[static_cast<std::size_t>(shadowLightIndex)].GetDirection();
    }

    for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
    {
        const Light& light = lights[lightIndex];
        if (light.GetType() == LightType::Directional)
        {
            return light.GetDirection();
        }
    }

    return glm::vec3(0.0f, 1.0f, 0.0f);
}

bool SceneRenderer::ComputeShadowCasterBounds(
    const Scene& scene,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax) const
{
    boundsMin = glm::vec3(std::numeric_limits<float>::max());
    boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    bool hasCasterBounds = false;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable() || !object.CastsShadow())
        {
            continue;
        }

        glm::vec3 objectBoundsMin;
        glm::vec3 objectBoundsMax;
        scene.GetWorldBounds(static_cast<int>(objectIndex), objectBoundsMin, objectBoundsMax);
        boundsMin = glm::min(boundsMin, objectBoundsMin);
        boundsMax = glm::max(boundsMax, objectBoundsMax);
        hasCasterBounds = true;
    }

    return hasCasterBounds;
}

void SceneRenderer::RenderShadowPass(const Scene& scene, const Camera& camera)
{
    EnsureGpuResources();
    if (!IsGpuResourcesReady())
    {
        return;
    }

    try
    {
    glm::vec3 casterBoundsMin;
    glm::vec3 casterBoundsMax;
    const bool hasCasterBounds = ComputeShadowCasterBounds(scene, casterBoundsMin, casterBoundsMax);
    if (!hasCasterBounds)
    {
        return;
    }

    m_shadowMap->SetResolution(m_directionalShadowSettings.GetShadowMapResolution());
    m_shadowMap->BeginFrame(
        camera,
        GetSunDirection(),
        casterBoundsMin,
        casterBoundsMax,
        true,
        m_directionalShadowSettings);

    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (int cascadeIndex = 0; cascadeIndex < m_shadowMap->GetActiveCascadeCount(); ++cascadeIndex)
    {
        m_shadowMap->BeginCascade(cascadeIndex);

        m_shadowDepthShader->SetMat4(
            "uLightSpaceMatrix",
            m_shadowMap->GetLightSpaceMatrix(cascadeIndex));

        const ShadowLightSpaceSetup& cascadeSetup =
            m_shadowMap->GetCascadeSetups()[static_cast<std::size_t>(cascadeIndex)];
        const float texelSpan =
            std::max(cascadeSetup.texelWorldSizeX, cascadeSetup.texelWorldSizeY);
        const float casterDepthBias = ComputeCasterDepthBiasNormalized(
            texelSpan,
            cascadeSetup.stableOrthoNear,
            cascadeSetup.stableOrthoFar,
            m_directionalShadowSettings.GetCasterDepthBiasScale());
        m_shadowDepthShader->SetFloat("uCasterDepthBias", casterDepthBias);
        m_shadowDepthShader->SetVec3("uLightDirectionTowardSource", GetSunDirection());

        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            const SceneObject& object = objects[objectIndex];
            if (!object.IsRenderable() || !object.CastsShadow())
            {
                continue;
            }

            const bool doubleSided = object.GetMaterial().IsDoubleSided();
            m_shadowDepthShader->Use(false, false, doubleSided);
            m_shadowDepthShader->SetMat4("uModel", scene.GetWorldMatrix(static_cast<int>(objectIndex)));
            m_shadowDepthShader->FlushUniforms();
            object.GetMesh()->Draw();
        }
    }
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("RenderShadowPass failed: ") + SafeExceptionMessage(exception));
    }
    catch (...)
    {
        throw std::runtime_error("RenderShadowPass failed: unknown error");
    }
}

void SceneRenderer::WarmUpDxrPipelineIfNeeded()
{
    if (!m_dxrSettings.IsEnabled() || !GfxContext::Get().IsRaytracingSupported())
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        DxrBreadcrumb("render: WarmUpDxrPipeline skipped (frame recording)");
        return;
    }

    try
    {
        const RenderDebugMode debugMode = m_screenSpaceEffects != nullptr
            ? m_screenSpaceEffects->GetDebugMode()
            : RenderDebugMode::None;
        const bool pathTracingActive = m_dxrSettings.IsPathTracingActive();
        const bool restirDiTemporalEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirDiTemporalEnabled()
            && m_dxrSettings.GetRestirDiCandidateCount() > 0;
        const bool restirGiTemporalEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirGiInitialEnabled()
            && m_dxrSettings.IsRestirGiTemporalEnabled();
        const bool restirGiSpatialEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirGiInitialEnabled()
            && m_dxrSettings.IsRestirGiSpatialEnabled();

        // Keep startup proportional to the project's active render path. Debug views remain
        // eligible so a saved debug mode is ready for the first frame.
        const bool warmSmoke = debugMode == RenderDebugMode::RtDispatchSmoke;
        const bool warmPrimary = m_dxrSettings.IsDebugTraceEnabled() || IsRtPrimaryDebugMode(debugMode);
        const bool warmReflections = !pathTracingActive
            && (m_dxrSettings.IsReflectionsEnabled() || IsRtReflectionDebugMode(debugMode));
        const bool warmShadows = !pathTracingActive
            && (m_dxrSettings.IsShadowsEnabled() || IsRtShadowDebugMode(debugMode));
        const bool warmGi = !pathTracingActive
            && (m_dxrSettings.IsGiEnabled() || IsRtGiDebugMode(debugMode));
        const bool warmPathTracer = pathTracingActive;
        const bool warmRestir = restirDiTemporalEnabled || restirGiTemporalEnabled
            || restirGiSpatialEnabled;

        const int pendingPipelineCount =
            (warmSmoke && (m_dxrSmokeDispatch == nullptr || !m_dxrSmokeDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmPrimary && (m_dxrPrimaryDebugDispatch == nullptr || !m_dxrPrimaryDebugDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmReflections && (m_dxrReflectionsDispatch == nullptr || !m_dxrReflectionsDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmShadows && (m_dxrShadowsDispatch == nullptr || !m_dxrShadowsDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmGi && (m_dxrGiDispatch == nullptr || !m_dxrGiDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmPathTracer && (m_dxrPathTracerDispatch == nullptr || !m_dxrPathTracerDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmRestir && (m_dxrRestirDispatch == nullptr || !m_dxrRestirDispatch->IsPipelineReady()) ? 1 : 0);
        if (pendingPipelineCount == 0)
        {
            return;
        }

        // Shader-library compilation is CPU work and does not touch the D3D12 device. Compile
        // every exact library permutation needed by this project before creating the dependent
        // RTPSOs in their normal (device-safe) order below. Keep the worker count bounded: a
        // project open must leave CPU time for the desktop, compositor, input, and UI threads.
        struct ShaderPrewarmRequest
        {
            const char* libraryPath = nullptr;
            DxrShaderLibraryCompileOptions options;
        };
        std::vector<ShaderPrewarmRequest> shaderPrewarmRequests;
        const auto queueShaderPrewarm = [&](const char* libraryPath, DxrShaderLibraryCompileOptions options) {
            shaderPrewarmRequests.push_back({libraryPath, std::move(options)});
        };
        const auto queueDefaultShaderPrewarm = [&](const char* libraryPath) {
            queueShaderPrewarm(libraryPath, DxrShaderCache::MakeActiveDeviceCompileOptions());
        };

        if (warmSmoke)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrSmokeLibraryShader);
        }
        if (warmPrimary)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrPrimaryDebugLibraryShader);
        }
        if (warmReflections)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrReflectionsLibraryShader);
        }
        if (warmShadows)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrShadowsLibraryShader);
        }
        if (warmGi)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrGiLibraryShader);
        }
        if (warmPathTracer)
        {
            queueShaderPrewarm(
                EngineConstants::DxrPathTracerLibraryShader,
                DxrShaderCache::MakeActiveDeviceCompileOptions(false));
            queueShaderPrewarm(
                EngineConstants::DxrPathTracerLibraryShader,
                DxrShaderCache::MakeActiveDeviceCompileOptions(true));
            if (GfxContext::Get().IsShaderExecutionReorderingSupported())
            {
                DxrShaderLibraryCompileOptions serOptions =
                    DxrShaderCache::MakeActiveDeviceCompileOptions(false);
                serOptions.serPermutation = true;
                serOptions.targetProfile = "lib_6_9";
                queueShaderPrewarm(EngineConstants::DxrPathTracerLibraryShader, serOptions);

                serOptions = DxrShaderCache::MakeActiveDeviceCompileOptions(true);
                serOptions.serPermutation = true;
                serOptions.targetProfile = "lib_6_9";
                queueShaderPrewarm(EngineConstants::DxrPathTracerLibraryShader, serOptions);
            }
        }
        if (warmRestir)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrRestirLibraryShader);
        }

        // PBR would otherwise compile for the first time while recording geometry after DXR is
        // already ready. Its bytecode/PSO construction is independent of DXR shader compilation,
        // so overlap it with that existing CPU warm-up and retain the result just long enough for
        // Material::EnsureShader() to take the cache reference in the first geometry pass.
        const bool needsPbrPrewarm = m_preWarmedPbrShader == nullptr;
        const std::size_t compilationWorkerBudget = BackgroundWork::ResponsiveWorkerCount(
            shaderPrewarmRequests.size() + (needsPbrPrewarm ? 1u : 0u),
            std::thread::hardware_concurrency());

        // Reserve one bounded worker for PBR while DXR libraries compile. On a one-worker system,
        // finish DXR first and compile PBR on the caller instead of exceeding the responsiveness
        // budget merely to overlap the two tasks.
        std::future<std::shared_ptr<Shader>> pbrPrewarmJob;
        const bool runPbrConcurrently = needsPbrPrewarm && compilationWorkerBudget > 1;
        if (runPbrConcurrently)
        {
            pbrPrewarmJob = std::async(std::launch::async, []() {
                BackgroundWork::LowerCurrentThreadPriority();
                return ShaderCache::Load(EngineConstants::LitVertexShader, EngineConstants::PbrFragmentShader);
            });
        }

        ProjectLoadBenchmark::ScopedPhase shaderPrewarmPhase("renderer.dxr_shader_library_prewarm");
        const int shaderLibraryJobCount = static_cast<int>(shaderPrewarmRequests.size());
        int completedShaderLibraryJobCount = 0;
        ProjectLoadProgress::Report(
            "Compiling ray tracing shader libraries (0/" + std::to_string(shaderLibraryJobCount) + ")...",
            ProjectLoadProgress::kDxrWarmupStart);

        const std::size_t dxrWorkerCount = std::max<std::size_t>(
            1,
            compilationWorkerBudget - (runPbrConcurrently ? 1u : 0u));
        std::atomic<std::size_t> nextShaderRequest{0};
        std::atomic<std::size_t> completedShaderRequests{0};
        std::atomic<bool> stopShaderWorkers{false};
        std::mutex shaderFailureMutex;
        std::exception_ptr shaderFailure;
        const auto compileShaderWorker = [&]() {
            BackgroundWork::LowerCurrentThreadPriority();
            while (!stopShaderWorkers.load(std::memory_order_acquire))
            {
                const std::size_t requestIndex = nextShaderRequest.fetch_add(1, std::memory_order_relaxed);
                if (requestIndex >= shaderPrewarmRequests.size())
                {
                    return;
                }

                try
                {
                    const ShaderPrewarmRequest& request = shaderPrewarmRequests[requestIndex];
                    (void)DxrShaderCache::Load(request.libraryPath, request.options);
                    completedShaderRequests.fetch_add(1, std::memory_order_release);
                }
                catch (...)
                {
                    {
                        std::lock_guard<std::mutex> lock(shaderFailureMutex);
                        if (shaderFailure == nullptr)
                        {
                            shaderFailure = std::current_exception();
                        }
                    }
                    stopShaderWorkers.store(true, std::memory_order_release);
                    return;
                }
            }
        };
        std::vector<std::future<void>> shaderPrewarmWorkers;
        shaderPrewarmWorkers.reserve(dxrWorkerCount);
        for (std::size_t workerIndex = 0; workerIndex < dxrWorkerCount; ++workerIndex)
        {
            shaderPrewarmWorkers.emplace_back(std::async(std::launch::async, compileShaderWorker));
        }
        while (completedShaderLibraryJobCount < shaderLibraryJobCount)
        {
            const int completedRequests = static_cast<int>(completedShaderRequests.load(std::memory_order_acquire));
            while (completedShaderLibraryJobCount < completedRequests)
            {
                ++completedShaderLibraryJobCount;
                ProjectLoadProgress::Report(
                    "Compiling ray tracing shader libraries ("
                        + std::to_string(completedShaderLibraryJobCount)
                        + "/" + std::to_string(shaderLibraryJobCount) + ")...",
                    ProjectLoadProgress::DxrShaderLibraryWarmup(
                        static_cast<float>(completedShaderLibraryJobCount)
                        / static_cast<float>(shaderLibraryJobCount)));
            }

            if (stopShaderWorkers.load(std::memory_order_acquire))
            {
                break;
            }

            // Compilation runs on worker threads. Yield briefly rather than spin while still
            // allowing the independently-owned native progress window to present completions.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        for (std::future<void>& worker : shaderPrewarmWorkers)
        {
            worker.get();
        }
        if (shaderFailure != nullptr)
        {
            std::rethrow_exception(shaderFailure);
        }

        if (needsPbrPrewarm)
        {
            ProjectLoadProgress::Report(
                "Preparing the first scene shader...",
                ProjectLoadProgress::kDxrShaderLibraryWarmupEnd);
            if (pbrPrewarmJob.valid())
            {
                m_preWarmedPbrShader = pbrPrewarmJob.get();
            }
            else
            {
                m_preWarmedPbrShader = ShaderCache::Load(
                    EngineConstants::LitVertexShader,
                    EngineConstants::PbrFragmentShader);
            }
        }

        int warmedPipelineCount = 0;
        const auto reportPipelineBegin = [&](const char* message) {
            const float progress = ProjectLoadProgress::DxrPipelineWarmup(
                static_cast<float>(warmedPipelineCount) / static_cast<float>(pendingPipelineCount));
            ProjectLoadProgress::Report(message, progress);
        };
        const auto markPipelineComplete = [&]() {
            ++warmedPipelineCount;
            const float progress = ProjectLoadProgress::DxrPipelineWarmup(
                static_cast<float>(warmedPipelineCount) / static_cast<float>(pendingPipelineCount));
            ProjectLoadProgress::SetProgress(progress);
        };
        const auto warmPipeline = [](const char* benchmarkPhase, auto&& callback) {
            ProjectLoadBenchmark::ScopedPhase phase(benchmarkPhase);
            callback();
        };

        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded begin");
        if (warmSmoke && (m_dxrSmokeDispatch == nullptr || !m_dxrSmokeDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR smoke pipeline...");
            if (m_dxrSmokeDispatch == nullptr)
            {
                m_dxrSmokeDispatch = std::make_unique<DxrSmokeDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.smoke", [&]() { m_dxrSmokeDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmPrimary && (m_dxrPrimaryDebugDispatch == nullptr || !m_dxrPrimaryDebugDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR primary-debug pipeline...");
            if (m_dxrPrimaryDebugDispatch == nullptr)
            {
                m_dxrPrimaryDebugDispatch = std::make_unique<DxrPrimaryDebugDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.primary_debug",
                [&]() { m_dxrPrimaryDebugDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmReflections && (m_dxrReflectionsDispatch == nullptr || !m_dxrReflectionsDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR reflections pipeline...");
            if (m_dxrReflectionsDispatch == nullptr)
            {
                m_dxrReflectionsDispatch = std::make_unique<DxrReflectionsDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.reflections",
                [&]() { m_dxrReflectionsDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmShadows && (m_dxrShadowsDispatch == nullptr || !m_dxrShadowsDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR shadows pipeline...");
            if (m_dxrShadowsDispatch == nullptr)
            {
                m_dxrShadowsDispatch = std::make_unique<DxrShadowsDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.shadows",
                [&]() { m_dxrShadowsDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmGi && (m_dxrGiDispatch == nullptr || !m_dxrGiDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR GI pipeline...");
            if (m_dxrGiDispatch == nullptr)
            {
                m_dxrGiDispatch = std::make_unique<DxrGiDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.gi", [&]() { m_dxrGiDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmPathTracer && (m_dxrPathTracerDispatch == nullptr || !m_dxrPathTracerDispatch->IsPipelineReady()))
        {
            if (m_dxrPathTracerDispatch == nullptr)
            {
                m_dxrPathTracerDispatch = std::make_unique<DxrPathTracerDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.path_tracer",
                [&]() {
                    m_dxrPathTracerDispatch->WarmUpPipelineIfNeeded(
                        [&](const int step, const int stepCount, const char* label) {
                            const float completedVariantFraction =
                                static_cast<float>(step - 1) / static_cast<float>(stepCount);
                            const float pipelineFraction =
                                (static_cast<float>(warmedPipelineCount) + completedVariantFraction)
                                / static_cast<float>(pendingPipelineCount);
                            ProjectLoadProgress::Report(
                                "Compiling path tracer pipeline (" + std::to_string(step) + "/"
                                    + std::to_string(stepCount) + "): " + label + "...",
                                ProjectLoadProgress::DxrPipelineWarmup(pipelineFraction));
                        });
                });
            markPipelineComplete();
        }
        if (warmRestir && (m_dxrRestirDispatch == nullptr || !m_dxrRestirDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling ReSTIR pipeline...");
            if (m_dxrRestirDispatch == nullptr)
            {
                m_dxrRestirDispatch = std::make_unique<DxrRestirDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.restir", [&]() { m_dxrRestirDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded end");
    }
    catch (const std::exception& exception)
    {
        EngineLog::Error(
            "dxr",
            std::string("WarmUpDxrPipelineIfNeeded failed: ") + SafeExceptionMessage(exception));
        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded failed");
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

void SceneRenderer::RecordDxrPass(
    const Scene& scene,
    const Camera& camera,
    const int dispatchWidth,
    const int dispatchHeight,
    const std::uintptr_t depthSrvCpuHandle,
    const bool usePostProcess,
    const RenderViewport renderViewport)
{
    const std::uint32_t pathTracerViewportId = RenderViewportHistoryId(renderViewport);
    if (!m_dxrSettings.IsEnabled() || !GfxContext::Get().IsRaytracingSupported())
    {
        // Master RT off (or unsupported): clear every DXR SRV + composite flag so the post stack
        // stops compositing STALE reflection/shadow/GI buffers from when RT was on (a ghost that
        // reprojects with the camera). The raster's spec-IBL omit is likewise gated on the master
        // toggle, so spec IBL is baked normally again.
        if (usePostProcess && m_screenSpaceEffects != nullptr)
        {
            m_screenSpaceEffects->SetDxrSmokeDebugSrv(0);
            m_screenSpaceEffects->SetDxrPrimaryDebugSrvs(0, 0);
            m_screenSpaceEffects->SetDxrReflectionSrv(0);
            m_screenSpaceEffects->SetDxrReflectionCompositeEnabled(false);
            m_screenSpaceEffects->SetDxrShadowSrv(0);
            m_screenSpaceEffects->SetDxrShadowCompositeEnabled(false);
            m_screenSpaceEffects->SetDxrGiSrv(0);
            m_screenSpaceEffects->SetDxrGiCompositeEnabled(false);
            m_screenSpaceEffects->SetDxrPathTracerDisplay(false, 0, 0);
        }
        return;
    }

    DxrBreadcrumb("render: EnsureScene begin");
    ProjectLoadProgress::Report(
        "Building ray tracing acceleration structures...",
        ProjectLoadProgress::kDxrAccelerationStructures);
    if (m_dxrAccelerationStructures == nullptr)
    {
        m_dxrAccelerationStructures = std::make_unique<DxrAccelerationStructures>();
    }

    const auto dxrScenePrepStart = std::chrono::steady_clock::now();
    {
        ProjectLoadBenchmark::ScopedPhase accelerationStructuresPhase(
            "renderer.first_scene.dxr_acceleration_structures");
        const GfxContext::GpuTimerScope gpuScope("DXR acceleration structures");
        m_dxrAccelerationStructures->EnsureScene(
            m_gpuScene,
            true,
            GfxContext::Get().GetCommandList());
    }
    m_renderFrameDiagnostics.dxrScenePrepCpuMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dxrScenePrepStart)
            .count();
    DxrBreadcrumb("render: EnsureScene end");
    ProjectLoadProgress::Report(
        "Dispatching ray tracing passes...",
        ProjectLoadProgress::kDxrDispatch);

    const RenderDebugMode debugMode =
        usePostProcess && m_screenSpaceEffects != nullptr ? m_screenSpaceEffects->GetDebugMode()
                                                          : RenderDebugMode::None;
    // DLSS Ray Reconstruction (devdoc/dxr-dlss-rr.md, RR2): when RR owns the resolve it replaces
    // the NRD denoisers — force NRD OFF for the RT signals it reconstructs (reflections + GI) so
    // the RAW noisy radiance flows into the HDR color. (RT shadows stay on SIGMA for now — open
    // decision #3.) RR3 wires the actual RR evaluate; until then RR-on shows the noisy signal.
    const bool rrActive = usePostProcess && m_screenSpaceEffects != nullptr
        && m_screenSpaceEffects->IsRayReconstructionActive();
    const bool smokeDebugMode = debugMode == RenderDebugMode::RtDispatchSmoke;
    const bool primaryDebugViewActive = IsRtPrimaryDebugMode(debugMode);
    const bool primaryTraceEnabled =
        m_dxrSettings.IsDebugTraceEnabled() || primaryDebugViewActive;

    // Every DXR export uses non-pixel shader SRVs. This must precede smoke/primary debug as well as
    // PT; doing it only immediately before PT leaves the earlier root-table bindings invalid.
    if (usePostProcess && m_screenSpaceEffects != nullptr)
    {
        m_screenSpaceEffects->PrepareSceneColorForDxrRead();
    }

    if (m_dxrSmokeDispatch == nullptr)
    {
        m_dxrSmokeDispatch = std::make_unique<DxrSmokeDispatch>();
    }

    DxrBreadcrumb("render: smoke DispatchIfEnabled begin");
    m_dxrSmokeDispatch->DispatchIfEnabled(
        *m_dxrAccelerationStructures,
        true,
        smokeDebugMode,
        GfxContext::Get().GetCommandList(),
        dispatchWidth,
        dispatchHeight);
    DxrBreadcrumb("render: smoke DispatchIfEnabled end");

    if (m_dxrPrimaryDebugDispatch == nullptr)
    {
        m_dxrPrimaryDebugDispatch = std::make_unique<DxrPrimaryDebugDispatch>();
    }

    DxrBreadcrumb("render: primary-debug DispatchIfEnabled begin");
    const bool primaryDebugDispatched = m_dxrPrimaryDebugDispatch->DispatchIfEnabled(
        *m_dxrAccelerationStructures,
        camera,
        true,
        m_dxrSettings.IsDebugTraceEnabled(),
        primaryDebugViewActive,
        GfxContext::Get().GetCommandList(),
        depthSrvCpuHandle,
        dispatchWidth,
        dispatchHeight,
        m_dxrSettings.GetMaxTraceDistance());
    DxrBreadcrumb("render: primary-debug DispatchIfEnabled end");

    // Phase P1 — path tracing (devdoc/dxr-path-tracing.md). When path tracing is the active rendering
    // mode, trace one camera ray per pixel and display direct-lit HDR radiance over the hybrid image.
    const bool pathTracingActive = m_dxrSettings.IsPathTracingActive();
    if (m_dxrPathTracerDispatch == nullptr)
    {
        m_dxrPathTracerDispatch = std::make_unique<DxrPathTracerDispatch>();
    }

    bool pathTracerDispatched = false;
    if (pathTracingActive && usePostProcess && m_screenSpaceEffects != nullptr
        && m_environmentMap != nullptr && m_environmentMap->GetIBL().IsReady())
    {
        // Post processing owns the PT output after RecordDxrPass and may leave it pixel-SRV-only
        // (notably the DLSS/RR resolve). Synchronize that final state back into the DXR owner before
        // it records this frame's SRV->UAV transition. Without this handoff the enhanced barriers
        // validator rejects the command list and shutdown cannot safely drain/release the RTPSO.
        m_dxrPathTracerDispatch->SetPrimaryOutputResourceState(
            pathTracerViewportId,
            m_screenSpaceEffects->GetPathTracerOutputResourceState());

        DxrBreadcrumb("render: path-tracer DispatchIfEnabled begin");
        const GfxContext::GpuTimerScope gpuScopePathTracer("Path tracer");

        const IBL& ptIbl = m_environmentMap->GetIBL();
        DxrPathTracerDispatch::FrameInputs ptInputs{};
        ptInputs.depthSrvCpuHandle = depthSrvCpuHandle;
        ptInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::ShadingNormal);
        ptInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
        ptInputs.directSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::DirectLighting);
        ptInputs.sunShadowSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::SunShadowFactor);
        ptInputs.indirectSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::IndirectLighting);
        ptInputs.prefilterSrvCpuHandle = ptIbl.GetPrefilterMapSrvCpuHandle();
        ptInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MotionVelocity);
        ptInputs.materialSrvIndex = m_dxrAccelerationStructures->GetMaterialSrvIndex();
        ptInputs.environmentIntensity = ptIbl.GetEnvironmentIntensity();
        ptInputs.environmentRotationYRadians = m_environmentMap->GetRotationYRadians();
        ptInputs.envEquirectSrvCpuHandle = ptIbl.GetHdrEquirectSrvCpuHandle();
        if (ptIbl.HasEnvImportanceSampling())
        {
            ptInputs.envImportanceCdfSrvIndex = ptIbl.GetEnvImportanceCdfSrvIndex();
            ptInputs.envImportanceSampleCount = ptIbl.GetEnvImportanceSampleCount();
            ptInputs.envImportanceCdfWidth = static_cast<std::uint32_t>(ptIbl.GetEnvImportanceCdfWidth());
            ptInputs.envImportanceCdfHeight = static_cast<std::uint32_t>(ptIbl.GetEnvImportanceCdfHeight());
            ptInputs.envImportanceWeightSum = ptIbl.GetEnvImportanceWeightSum();
            ptInputs.envDirectLightingLuminanceClamp = ptIbl.GetEnvDirectLightingLuminanceClamp();
        }
        ptInputs.maxReflectionLod = ptIbl.GetMaxReflectionLod();
        {
            const IrradianceSh9& ptSh9 = ptIbl.GetIrradianceSh9();
            for (std::size_t i = 0;
                 i < ptSh9.coefficients.size() && i < ptInputs.irradianceSh9.size();
                 ++i)
            {
                ptInputs.irradianceSh9[i] = ptSh9.coefficients[i];
            }
        }
        {
            const MotionVectorFrameState& motionState = m_screenSpaceEffects->GetMotionVectorFrameState();
            const glm::mat4 currentView = camera.GetViewMatrix();
            const glm::mat4 currentProjection = camera.GetUnjitteredProjectionMatrix();
            ptInputs.cameraPacket.current = TemporalCamera::MakeState(
                currentView,
                currentProjection,
                glm::inverse(currentProjection * currentView),
                camera.GetPosition(),
                camera.GetProjectionJitter());
            ptInputs.cameraPacket.previous = motionState.previousCamera;

            // Raster motion fields remain independent consumers for now. When their history is
            // declared valid, assert that every retained duplicate describes this packet's same
            // previous compatible rendered frame for the active Scene/Game viewport.
            const bool duplicateHistoryAgrees = !motionState.historyValid
                || (TemporalCamera::IsComplete(motionState.previousCamera)
                    && TemporalCamera::NearlyEqual(
                        motionState.previousCamera.view,
                        motionState.prevView)
                    && TemporalCamera::NearlyEqual(
                        motionState.previousCamera.projection,
                        motionState.prevUnjitteredProjection)
                    && TemporalCamera::NearlyEqual(
                        TemporalCamera::ApplyJitter(
                            motionState.previousCamera.projection,
                            motionState.previousCamera.jitterNdc),
                        motionState.prevProjection)
                    && TemporalCamera::NearlyEqual(
                        motionState.previousCamera.projection
                            * motionState.previousCamera.view,
                        motionState.prevViewProjection));
            assert(duplicateHistoryAgrees && "Viewport camera-history duplicates disagree");
            if (!motionState.historyValid || !duplicateHistoryAgrees)
            {
                ptInputs.cameraPacket.previous.valid = false;
            }
        }
        ptInputs.centerPrimaryRays = !m_dxrSettings.IsPtReferenceConvergence();
        ptInputs.restirDiCandidateCount =
            static_cast<std::uint32_t>(std::max(0, m_dxrSettings.GetRestirDiCandidateCount()));
        ptInputs.restirGiInitialEnabled = m_dxrSettings.IsRestirGiInitialEnabled();
        ptInputs.sunDirection = glm::normalize(GetSunDirection());
        ptInputs.sunAngularRadiusDegrees = m_dxrSettings.GetSunAngularRadiusDegrees();
        {
            const std::vector<Light>& lights = m_lighting.GetLights();
            const int shadowLightIndex = m_lighting.GetShadowLightIndex();
            const Light* sun = nullptr;
            if (shadowLightIndex >= 0
                && static_cast<std::size_t>(shadowLightIndex) < lights.size()
                && lights[static_cast<std::size_t>(shadowLightIndex)].GetType() == LightType::Directional)
            {
                sun = &lights[static_cast<std::size_t>(shadowLightIndex)];
            }
            else
            {
                for (const Light& light : lights)
                {
                    if (light.GetType() == LightType::Directional)
                    {
                        sun = &light;
                        break;
                    }
                }
            }
            if (sun != nullptr)
            {
                ptInputs.sunColor = sun->GetColor();
                ptInputs.sunIntensity = sun->GetIntensity();
            }
        }

        const int ptDebugMode = PtDebugIsolateModeFromRenderDebug(debugMode);

        pathTracerDispatched = m_dxrPathTracerDispatch->DispatchIfEnabled(
            pathTracerViewportId,
            *m_dxrAccelerationStructures,
            camera,
            true,
            true,
            GfxContext::Get().GetCommandList(),
            ptInputs,
            dispatchWidth,
            dispatchHeight,
            dispatchWidth,
            dispatchHeight,
            m_dxrSettings.GetMaxTraceDistance(),
            m_dxrSettings.GetPtMaxBounces(),
            m_dxrSettings.IsPtRussianRouletteEnabled(),
            m_dxrSettings.IsPtFireflyClampEnabled(),
            m_dxrSettings.IsPtDeterministicOpticalSplitEnabled(),
            m_dxrSettings.IsPtIndependentOpticalRrLayersEnabled(),
            m_dxrSettings.IsPtOpticalMotionReplayEnabled(),
            m_dxrSettings.GetPtAmbientStrength(),
            m_dxrSettings.GetPtAmbientAoRayCount(),
            ptDebugMode);
        DxrBreadcrumb("render: path-tracer DispatchIfEnabled end");

        if (pathTracerDispatched)
        {
            const bool diTemporalEnabled = !m_dxrSettings.IsPtReferenceConvergence()
                && m_dxrSettings.IsRestirDiTemporalEnabled()
                && m_dxrSettings.GetRestirDiCandidateCount() > 0;
            const bool giTemporalEnabled = !m_dxrSettings.IsPtReferenceConvergence()
                && m_dxrSettings.IsRestirGiInitialEnabled()
                && m_dxrSettings.IsRestirGiTemporalEnabled();
            bool giBoilingFilterEnabled = m_dxrSettings.IsRestirGiSpatialEnabled();
            bool giSpatialReuseEnabled = m_dxrSettings.IsRestirGiSpatialEnabled();
            switch (m_dxrSettings.GetRestirGiSpatialDiagnosticMode())
            {
            case RestirGiSpatialDiagnosticMode::Baseline:
                giBoilingFilterEnabled = false;
                giSpatialReuseEnabled = false;
                break;
            case RestirGiSpatialDiagnosticMode::FilterOnly:
                giBoilingFilterEnabled = true;
                giSpatialReuseEnabled = false;
                break;
            case RestirGiSpatialDiagnosticMode::SpatialOnly:
                giBoilingFilterEnabled = false;
                giSpatialReuseEnabled = true;
                break;
            case RestirGiSpatialDiagnosticMode::Full:
                giBoilingFilterEnabled = true;
                giSpatialReuseEnabled = true;
                break;
            case RestirGiSpatialDiagnosticMode::Production:
            default:
                break;
            }
            giBoilingFilterEnabled = !m_dxrSettings.IsPtReferenceConvergence()
                && m_dxrSettings.IsRestirGiInitialEnabled() && giBoilingFilterEnabled;
            giSpatialReuseEnabled = !m_dxrSettings.IsPtReferenceConvergence()
                && m_dxrSettings.IsRestirGiInitialEnabled() && giSpatialReuseEnabled;
            const bool giSpatialEnabled = giBoilingFilterEnabled || giSpatialReuseEnabled;
            const bool giSpatialMeasurement = !m_dxrSettings.IsPtReferenceConvergence()
                && m_dxrSettings.IsRestirGiInitialEnabled()
                && IsPtRestirGiSpatialStatsDebugMode(debugMode);
            // The temporal pass is also the fresh-reservoir copy into the spatial read slot, so P7
            // can operate independently when P6 is disabled.
            const bool temporalEnabled = (diTemporalEnabled || giTemporalEnabled || giSpatialEnabled
                    || giSpatialMeasurement)
                && m_dxrRestirDispatch != nullptr;
            if (temporalEnabled)
            {
                const bool shadeRestirOutput = ptDebugMode == 0;
                const bool temporalSucceeded = m_dxrPathTracerDispatch->DispatchRestirTemporal(
                    pathTracerViewportId,
                    *m_dxrRestirDispatch,
                    *m_dxrAccelerationStructures,
                    camera,
                    GfxContext::Get().GetCommandList(),
                    m_dxrSettings.GetMaxTraceDistance(),
                    m_dxrAccelerationStructures->GetPtSceneVersion(),
                    m_dxrAccelerationStructures->GetPtMotionVersion(),
                    true,
                    diTemporalEnabled,
                    giTemporalEnabled,
                    shadeRestirOutput);
                if (!temporalSucceeded)
                {
                    m_dxrPathTracerDispatch->InvalidateRestirHistory(pathTracerViewportId);
                }
                else if ((diTemporalEnabled || giSpatialEnabled || giSpatialMeasurement)
                    && !m_dxrPathTracerDispatch->DispatchRestirSpatial(
                        pathTracerViewportId,
                        *m_dxrRestirDispatch,
                        *m_dxrAccelerationStructures,
                        camera,
                        GfxContext::Get().GetCommandList(),
                        m_dxrSettings.GetMaxTraceDistance(),
                        true,
                        diTemporalEnabled,
                        giBoilingFilterEnabled,
                        giSpatialReuseEnabled,
                        shadeRestirOutput))
                {
                    m_dxrPathTracerDispatch->InvalidateRestirHistory(pathTracerViewportId);
                }
            }
            else
            {
                m_dxrPathTracerDispatch->InvalidateRestirHistory(pathTracerViewportId);
            }
            {
                const GfxContext::GpuTimerScope gpuScopePtHistory("Path tracer/Surface history");
                m_dxrPathTracerDispatch->FinalizePathTracerSurfaceHistory(
                    pathTracerViewportId,
                    GfxContext::Get().GetCommandList());
            }
        }

        if (pathTracerDispatched && m_dxrSettings.IsPtReferenceConvergence())
        {
            PathTracerHistoryKey historyKey{};
            historyKey.viewProjection =
                camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix();
            historyKey.width = dispatchWidth;
            historyKey.height = dispatchHeight;
            historyKey.convergenceMode = m_dxrSettings.GetPtConvergenceMode();
            historyKey.maxTraceDistance = m_dxrSettings.GetMaxTraceDistance();
            historyKey.sunDirection = ptInputs.sunDirection;
            historyKey.sunColor = ptInputs.sunColor;
            historyKey.sunIntensity = ptInputs.sunIntensity;
            historyKey.environmentIntensity = ptInputs.environmentIntensity;
            historyKey.geometryObjectCount = m_dxrAccelerationStructures->GetGeometryObjectCount();

            m_screenSpaceEffects->AccumulatePathTracerReference(
                historyKey,
                m_dxrPathTracerDispatch->GetPrimaryOutputSrvCpuHandle(pathTracerViewportId),
                dispatchWidth,
                dispatchHeight);
        }
    }

    if (usePostProcess && m_screenSpaceEffects != nullptr)
    {
        // A zero-instance scene deliberately has no TLAS, so the PT dispatch is skipped.  Its
        // output resources remain allocated for reuse when geometry returns, but their contents
        // are from the last populated frame.  Only present an output written this frame; otherwise
        // let the normal composite draw the camera-relative environment background.
        const bool pathTracerShow = pathTracingActive && pathTracerDispatched;
        m_screenSpaceEffects->SetPtRrBundleMode(m_dxrSettings.GetPtRrBundleMode());
        m_screenSpaceEffects->SetPtIndependentOpticalRrLayersEnabled(
            m_dxrSettings.IsPtIndependentOpticalRrLayersEnabled());
        m_screenSpaceEffects->SetDxrPathTracerDisplay(
            pathTracerShow,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPrimaryOutputSrvCpuHandle(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPrimaryMetadataSrvCpuHandle(pathTracerViewportId)
                : 0,
            m_dxrSettings.GetPtConvergenceMode(),
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPrimaryOutputResource(pathTracerViewportId)
                : nullptr,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPrimaryOutputResourceState(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerDepthResource(pathTracerViewportId)
                : nullptr,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerDepthResourceState(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerMotionResource(pathTracerViewportId)
                : nullptr,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerMotionResourceState(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerDepthSrvCpuHandle(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerMotionSrvCpuHandle(pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerDiffuseAlbedoSrvCpuHandle(
                    pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerSpecularAlbedoSrvCpuHandle(
                    pathTracerViewportId)
                : 0,
            pathTracerShow
                ? m_dxrPathTracerDispatch->GetPathTracerNormalRoughnessSrvCpuHandle(
                    pathTracerViewportId)
                : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionOutputResource(pathTracerViewportId) : nullptr,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionOutputResourceState(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionOutputSrvCpuHandle(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionDepthResource(pathTracerViewportId) : nullptr,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionDepthResourceState(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionDepthSrvCpuHandle(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionMotionResource(pathTracerViewportId) : nullptr,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionMotionResourceState(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionMotionSrvCpuHandle(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionDiffuseAlbedoSrvCpuHandle(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionSpecularAlbedoSrvCpuHandle(pathTracerViewportId) : 0,
            pathTracerShow ? m_dxrPathTracerDispatch->GetPathTracerOpticalTransmissionNormalRoughnessSrvCpuHandle(pathTracerViewportId) : 0);
    }

    // Phase D9 — RT diffuse GI trace (devdoc/dxr-diffuse-gi.md). Runs before reflections so
    // reflection hits know GI is enabled; bounce lighting is traced world-space at each hit.
    const bool giDebugViewActive = IsRtGiDebugMode(debugMode);
    const bool giWanted =
        !pathTracingActive && (m_dxrSettings.IsGiEnabled() || giDebugViewActive);
    bool giDispatched = false;

    if (m_dxrGiDispatch == nullptr)
    {
        m_dxrGiDispatch = std::make_unique<DxrGiDispatch>();
    }

    if (giWanted && usePostProcess && m_screenSpaceEffects != nullptr
        && m_environmentMap != nullptr && m_environmentMap->GetIBL().IsReady())
    {
        DxrBreadcrumb("render: gi DispatchIfEnabled begin");
        const GfxContext::GpuTimerScope gpuScopeGi("RT diffuse GI");

        float giQualityScale = 0.75f; // Medium
        switch (m_dxrSettings.GetReflectionsQuality())
        {
        case DxrReflectionsQuality::Low:
            giQualityScale = 0.5f;
            break;
        case DxrReflectionsQuality::High:
            giQualityScale = 1.0f;
            break;
        case DxrReflectionsQuality::Medium:
        default:
            giQualityScale = 0.75f;
            break;
        }

        const int giWidth =
            std::max(1, static_cast<int>(static_cast<float>(dispatchWidth) * giQualityScale));
        const int giHeight =
            std::max(1, static_cast<int>(static_cast<float>(dispatchHeight) * giQualityScale));

        const IBL& giIbl = m_environmentMap->GetIBL();
        DxrGiDispatch::FrameInputs giInputs{};
        giInputs.depthSrvCpuHandle = depthSrvCpuHandle;
        giInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::ShadingNormal);
        giInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
        giInputs.directSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::DirectLighting);
        giInputs.sunShadowSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::SunShadowFactor);
        giInputs.indirectSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::IndirectLighting);
        giInputs.prefilterSrvCpuHandle = giIbl.GetPrefilterMapSrvCpuHandle();
        giInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MotionVelocity);
        giInputs.environmentIntensity = giIbl.GetEnvironmentIntensity();
        giInputs.maxReflectionLod = giIbl.GetMaxReflectionLod();

        giInputs.materialSrvIndex = m_dxrAccelerationStructures->GetMaterialSrvIndex();
        giInputs.sunDirection = glm::normalize(GetSunDirection());
        {
            const std::vector<Light>& lights = m_lighting.GetLights();
            const int shadowLightIndex = m_lighting.GetShadowLightIndex();
            const Light* sun = nullptr;
            if (shadowLightIndex >= 0
                && static_cast<std::size_t>(shadowLightIndex) < lights.size()
                && lights[static_cast<std::size_t>(shadowLightIndex)].GetType() == LightType::Directional)
            {
                sun = &lights[static_cast<std::size_t>(shadowLightIndex)];
            }
            else
            {
                for (const Light& light : lights)
                {
                    if (light.GetType() == LightType::Directional)
                    {
                        sun = &light;
                        break;
                    }
                }
            }
            if (sun != nullptr)
            {
                giInputs.sunColor = sun->GetColor();
                giInputs.sunIntensity = sun->GetIntensity();
            }
        }
        const IrradianceSh9& giSh9 = giIbl.GetIrradianceSh9();
        for (std::size_t i = 0; i < giSh9.coefficients.size() && i < giInputs.irradianceSh9.size(); ++i)
        {
            giInputs.irradianceSh9[i] = giSh9.coefficients[i];
        }

        m_screenSpaceEffects->PrepareSceneColorForDxrRead();

        giDispatched = m_dxrGiDispatch->DispatchIfEnabled(
            *m_dxrAccelerationStructures,
            camera,
            true,
            m_dxrSettings.IsGiEnabled(),
            giDebugViewActive,
            GfxContext::Get().GetCommandList(),
            giInputs,
            giWidth,
            giHeight,
            dispatchWidth,
            dispatchHeight,
            m_dxrSettings.GetMaxTraceDistance(),
            1, // RELAX_DIFFUSE is designed for 1 spp
            m_dxrSettings.IsGiDenoiseEnabled() && !rrActive,
            m_dxrSettings.GetTemporalBlend(),
            m_dxrSettings.GetReflectionAtrousIterations(),
            m_dxrSettings.IsReflectionAntiFireflyEnabled());
        DxrBreadcrumb("render: gi DispatchIfEnabled end");
    }

    // Phase D4 — reflection trace (devdoc/dxr-reflections.md). Requires the scene MRTs, so
    // only runs on the post-process path.
    const bool reflectionDebugViewActive = IsRtReflectionDebugMode(debugMode);
    const bool reflectionsWanted =
        !pathTracingActive
        && (m_dxrSettings.IsReflectionsEnabled() || reflectionDebugViewActive);
    bool reflectionsDispatched = false;

    if (m_dxrReflectionsDispatch == nullptr)
    {
        m_dxrReflectionsDispatch = std::make_unique<DxrReflectionsDispatch>();
    }

    if (reflectionsWanted && usePostProcess && m_screenSpaceEffects != nullptr
        && m_environmentMap != nullptr && m_environmentMap->GetIBL().IsReady())
    {
        DxrBreadcrumb("render: reflections DispatchIfEnabled begin");
        const GfxContext::GpuTimerScope gpuScopeReflections("RT reflections");

        float qualityScale = 0.75f; // Medium
        switch (m_dxrSettings.GetReflectionsQuality())
        {
        case DxrReflectionsQuality::Low:
            qualityScale = 0.5f;
            break;
        case DxrReflectionsQuality::High:
            qualityScale = 1.0f;
            break;
        case DxrReflectionsQuality::Medium:
        default:
            qualityScale = 0.75f;
            break;
        }

        const int reflectionWidth =
            std::max(1, static_cast<int>(static_cast<float>(dispatchWidth) * qualityScale));
        const int reflectionHeight =
            std::max(1, static_cast<int>(static_cast<float>(dispatchHeight) * qualityScale));

        const IBL& ibl = m_environmentMap->GetIBL();
        DxrReflectionsDispatch::FrameInputs frameInputs{};
        frameInputs.depthSrvCpuHandle = depthSrvCpuHandle;
        frameInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::ShadingNormal);
        frameInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
        frameInputs.directSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::DirectLighting);
        frameInputs.sunShadowSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::SunShadowFactor);
        frameInputs.indirectSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::IndirectLighting);
        frameInputs.prefilterSrvCpuHandle = ibl.GetPrefilterMapSrvCpuHandle();
        frameInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MotionVelocity);
        frameInputs.giStrength = m_dxrSettings.GetGiStrength();
        frameInputs.hasGiTrace = giDispatched && m_dxrSettings.IsGiEnabled();
        frameInputs.sunAngularRadiusDegrees = m_dxrSettings.GetSunAngularRadiusDegrees();
        frameInputs.environmentIntensity = ibl.GetEnvironmentIntensity();
        frameInputs.maxReflectionLod = ibl.GetMaxReflectionLod();

        // In-hit analytic shading inputs. Sun matches the primary directional light used by the
        // raster PBR pass (uLightDirections = normalize(GetDirection()); color is sRGB).
        frameInputs.materialSrvIndex = m_dxrAccelerationStructures->GetMaterialSrvIndex();
        frameInputs.sunDirection = glm::normalize(GetSunDirection());
        {
            const std::vector<Light>& lights = m_lighting.GetLights();
            const int shadowLightIndex = m_lighting.GetShadowLightIndex();
            const Light* sun = nullptr;
            if (shadowLightIndex >= 0
                && static_cast<std::size_t>(shadowLightIndex) < lights.size()
                && lights[static_cast<std::size_t>(shadowLightIndex)].GetType() == LightType::Directional)
            {
                sun = &lights[static_cast<std::size_t>(shadowLightIndex)];
            }
            else
            {
                for (const Light& light : lights)
                {
                    if (light.GetType() == LightType::Directional)
                    {
                        sun = &light;
                        break;
                    }
                }
            }
            if (sun != nullptr)
            {
                frameInputs.sunColor = sun->GetColor();
                frameInputs.sunIntensity = sun->GetIntensity();
            }
        }
        const IrradianceSh9& sh9 = ibl.GetIrradianceSh9();
        for (std::size_t i = 0; i < sh9.coefficients.size() && i < frameInputs.irradianceSh9.size(); ++i)
        {
            frameInputs.irradianceSh9[i] = sh9.coefficients[i];
        }

        // DispatchRays samples the MRTs from non-pixel shaders; move them (tracked) to a
        // combined read state first.
        m_screenSpaceEffects->PrepareSceneColorForDxrRead();

        reflectionsDispatched = m_dxrReflectionsDispatch->DispatchIfEnabled(
            *m_dxrAccelerationStructures,
            camera,
            true,
            m_dxrSettings.IsReflectionsEnabled(),
            reflectionDebugViewActive,
            GfxContext::Get().GetCommandList(),
            frameInputs,
            reflectionWidth,
            reflectionHeight,
            dispatchWidth,
            dispatchHeight,
            m_dxrSettings.GetMaxTraceDistance(),
            m_dxrSettings.GetReflectionsSamplesPerPixel(),
            m_dxrSettings.IsDenoiseEnabled() && !rrActive, // RR reconstructs the raw signal instead
            m_dxrSettings.GetTemporalBlend(),
            m_dxrSettings.GetReflectionAtrousIterations(),
            m_dxrSettings.IsReflectionAntiFireflyEnabled(),
            m_dxrSettings.GetReflectionAoRays(),
            m_dxrSettings.GetReflectionRoughnessCutoff());
        DxrBreadcrumb("render: reflections DispatchIfEnabled end");
    }

    // Phase D8 — RT sun shadow trace (devdoc/dxr-shadows.md). Full render resolution; needs the
    // scene MRTs (depth, RT2 normal, RT5 roughness, RT4 velocity) so only runs on post-process.
    const bool shadowDebugViewActive = IsRtShadowDebugMode(debugMode);
    const bool shadowsWanted =
        !pathTracingActive && (m_dxrSettings.IsShadowsEnabled() || shadowDebugViewActive);
    bool shadowsDispatched = false;

    if (m_dxrShadowsDispatch == nullptr)
    {
        m_dxrShadowsDispatch = std::make_unique<DxrShadowsDispatch>();
    }

    if (shadowsWanted && usePostProcess && m_screenSpaceEffects != nullptr)
    {
        DxrBreadcrumb("render: shadows DispatchIfEnabled begin");
        const GfxContext::GpuTimerScope gpuScopeShadows("RT shadows");

        DxrShadowsDispatch::FrameInputs shadowInputs{};
        shadowInputs.depthSrvCpuHandle = depthSrvCpuHandle;
        shadowInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::ShadingNormal);
        shadowInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
        shadowInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(GBufferSlot::MotionVelocity);
        shadowInputs.sunDirection = glm::normalize(GetSunDirection());
        shadowInputs.sunAngularRadiusDegrees = m_dxrSettings.GetSunAngularRadiusDegrees();

        // DispatchRays samples the MRTs from non-pixel shaders; move them (tracked) to a combined
        // read state first. Idempotent with the reflection pass's transition.
        m_screenSpaceEffects->PrepareSceneColorForDxrRead();

        shadowsDispatched = m_dxrShadowsDispatch->DispatchIfEnabled(
            *m_dxrAccelerationStructures,
            camera,
            true,
            m_dxrSettings.IsShadowsEnabled(),
            shadowDebugViewActive,
            GfxContext::Get().GetCommandList(),
            shadowInputs,
            dispatchWidth,
            dispatchHeight,
            dispatchWidth,
            dispatchHeight,
            m_dxrSettings.GetMaxTraceDistance(),
            m_dxrSettings.IsShadowDenoiseEnabled());
        DxrBreadcrumb("render: shadows DispatchIfEnabled end");
    }

    if (usePostProcess && m_screenSpaceEffects != nullptr)
    {
        DxrBreadcrumb("render: SetDxrDebugSrvs");
        m_screenSpaceEffects->SetDxrSmokeDebugSrv(
            m_dxrSmokeDispatch->HasValidOutput() ? m_dxrSmokeDispatch->GetOutputSrvCpuHandle() : 0);
        const bool hasFreshPrimaryOutput =
            primaryDebugDispatched && m_dxrPrimaryDebugDispatch->HasValidOutput();
        if (hasFreshPrimaryOutput)
        {
            m_screenSpaceEffects->NotifyRtPrimaryDebugDispatched();
        }

        m_screenSpaceEffects->SetDxrPrimaryDebugSrvs(
            primaryTraceEnabled && hasFreshPrimaryOutput
                ? m_dxrPrimaryDebugDispatch->GetPrimaryOutputSrvCpuHandle()
                : 0,
            primaryTraceEnabled && hasFreshPrimaryOutput
                ? m_dxrPrimaryDebugDispatch->GetPrimaryMetadataSrvCpuHandle()
                : 0);

        m_screenSpaceEffects->SetDxrReflectionSrv(
            reflectionsDispatched && m_dxrReflectionsDispatch->HasValidOutput()
                ? m_dxrReflectionsDispatch->GetReflectionOutputSrvCpuHandle()
                : 0,
            m_dxrReflectionsDispatch->GetOutputUvScaleX(),
            m_dxrReflectionsDispatch->GetOutputUvScaleY(),
            reflectionsDispatched ? m_dxrReflectionsDispatch->GetDenoisedSrvCpuHandle() : 0,
            m_dxrSettings.GetMaxTraceDistance());

        // Run the RT specular composite in Apply (and skip SSR's) whenever RT reflections are
        // ENABLED — not only on a fresh trace. The raster omits spec IBL when the composite will
        // add it back (see the IBL omit flag set before the scene draw), so the composite MUST
        // run to re-add it; it falls back to pure IBL (uHasRtTrace=0) when there is no fresh trace.
        m_screenSpaceEffects->SetDxrReflectionCompositeEnabled(m_dxrSettings.IsReflectionsEnabled());
        m_screenSpaceEffects->SetDxrReflectionRoughnessCutoff(
            m_dxrSettings.GetReflectionRoughnessCutoff());

        // D8: RT sun shadow mask (penumbra drives the raw debug view, denoised feeds composite).
        m_screenSpaceEffects->SetDxrShadowSrv(
            shadowsDispatched && m_dxrShadowsDispatch->HasValidOutput()
                ? m_dxrShadowsDispatch->GetPenumbraSrvCpuHandle()
                : 0,
            shadowsDispatched ? m_dxrShadowsDispatch->GetDenoisedSrvCpuHandle() : 0,
            m_dxrShadowsDispatch->GetOutputUvScaleX(),
            m_dxrShadowsDispatch->GetOutputUvScaleY());

        // Replace the CSM sun shadow factor with the RT mask only when the user enabled RT
        // shadows AND a fresh denoised mask exists this frame.
        m_screenSpaceEffects->SetDxrShadowCompositeEnabled(
            m_dxrSettings.IsShadowsEnabled() && shadowsDispatched
            && m_dxrShadowsDispatch->DenoisedThisFrame());

        // D9: RT diffuse GI (raw drives the raw debug view, denoised feeds the inject pass).
        m_screenSpaceEffects->SetDxrGiSrv(
            giDispatched && m_dxrGiDispatch->HasValidOutput()
                ? m_dxrGiDispatch->GetGiOutputSrvCpuHandle()
                : 0,
            giDispatched ? m_dxrGiDispatch->GetDenoisedSrvCpuHandle() : 0,
            m_dxrGiDispatch->GetOutputUvScaleX(),
            m_dxrGiDispatch->GetOutputUvScaleY());
        m_screenSpaceEffects->SetDxrGiStrength(m_dxrSettings.GetGiStrength());

        // Run the GI inject (and skip SSGI inject) whenever RT GI is ENABLED — not only on a fresh
        // trace. The raster omits the SH diffuse ambient when GI is enabled, so the inject MUST run
        // to replace it; it recomputes a transient SH ambient (uHasGiTrace=0) when there's no trace.
        m_screenSpaceEffects->SetDxrGiCompositeEnabled(m_dxrSettings.IsGiEnabled());
    }
}

void SceneRenderer::PrepareSceneRasterTarget(
    const Scene& scene,
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    Framebuffer* target,
    const bool usePostProcess,
    const bool freezeTemporalJitter,
    bool& outSplitLightingMrt)
{
    (void)scene;

    if (usePostProcess)
    {
        SceneRenderTrace::Section aaSection("aa");
        Camera& antiAliasCamera = const_cast<Camera&>(camera);
        {
            SceneRenderTrace::Scope resizeScope("ScreenSpaceEffects::Resize");
            m_screenSpaceEffects->Resize(viewportWidth, viewportHeight);
            resizeScope.Success();
        }
        antiAliasCamera.SetAspectFromFramebuffer(
            m_screenSpaceEffects->GetRenderWidth(),
            m_screenSpaceEffects->GetRenderHeight());
        SyncEffectiveMaterialMipBias();
        {
            SceneRenderTrace::Scope prepareAaScope("PrepareAntiAliasingFrame");
            m_screenSpaceEffects->PrepareAntiAliasingFrame(antiAliasCamera, freezeTemporalJitter);
            prepareAaScope.Success();
        }
        {
            SceneRenderTrace::Scope beginPassScope("BeginScenePass");
            m_screenSpaceEffects->BeginScenePass(*m_environmentMap);
            beginPassScope.Success();
        }
        aaSection.Success();
        outSplitLightingMrt = m_screenSpaceEffects->HasSplitLighting();
        return;
    }

    if (target != nullptr)
    {
        const glm::vec3 solidSrgb = m_environmentMap->GetSolidBackgroundColorSrgb();
        const glm::vec3 solidBackground = glm::vec3(
            std::pow(solidSrgb.x, 2.2f),
            std::pow(solidSrgb.y, 2.2f),
            std::pow(solidSrgb.z, 2.2f));
        const float solidClear[] = {solidBackground.x, solidBackground.y, solidBackground.z, 1.0f};
        const float blackClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float* clearColor =
            m_environmentMap->UsesSolidColorBackground() ? solidClear : blackClear;
        target->BindDrawTarget(true, clearColor);
        GfxContext::Get().SetBoundOutputFramebuffer(target);
        outSplitLightingMrt = target->HasSplitLighting();
        return;
    }

    outSplitLightingMrt = false;
}

void SceneRenderer::RenderGeometryPass(
    const Scene& scene,
    const Camera& camera,
    const bool usePostProcess,
    Framebuffer* target,
    const bool splitLightingMrt,
    const RenderDebugMode materialDebugMode,
    const RenderDebugMode activeDebugMode)
{
    if (!m_dxrSettings.IsPathTracingActive())
    {
        SceneRenderTrace::Scope skyboxScope("RenderSkybox");
        m_environmentMap->RenderSkybox(camera, splitLightingMrt);
        skyboxScope.Success();
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
    const GpuScene::PreviousWorldMap& previousWorld = m_activePreviousWorldByObjectId != nullptr
        ? *m_activePreviousWorldByObjectId
        : m_previousWorldByObjectId;

    const MotionVectorFrameState motionFrameState =
        usePostProcess ? m_screenSpaceEffects->GetMotionVectorFrameState() : MotionVectorFrameState{};

    if (m_environmentMap != nullptr)
    {
        const bool iblReady = m_environmentMap->GetIBL().IsReady();
        const bool dxrReflectionsActive =
            m_dxrSettings.IsEnabled() && m_dxrSettings.IsReflectionsEnabled();
        const bool omitSpecIbl = usePostProcess && m_screenSpaceEffects != nullptr
            && m_screenSpaceEffects->ReflectionCompositeReplacesSpecIbl(
                   dxrReflectionsActive, iblReady, activeDebugMode);
        m_environmentMap->GetIBL().SetReflectionsReplaceSpecIbl(omitSpecIbl);

        const bool dxrGiActive = m_dxrSettings.IsEnabled() && m_dxrSettings.IsGiEnabled();
        const bool omitDiffuseIbl = usePostProcess && m_screenSpaceEffects != nullptr
            && m_screenSpaceEffects->GiInjectReplacesDiffuseIbl(
                   dxrGiActive, iblReady, activeDebugMode);
        m_environmentMap->GetIBL().SetGiReplacesDiffuseIbl(omitDiffuseIbl);
    }

    {
        const auto rasterRecordStart = std::chrono::steady_clock::now();
        SceneRenderTrace::Scope drawScope("draw scene objects");
        const GfxContext::GpuTimerScope gpuScopeRaster("Scene raster");
        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            const SceneObject& object = objects[objectIndex];
            if (!object.IsRenderable())
            {
                continue;
            }

            const glm::mat4 modelMatrix = scene.GetWorldMatrix(static_cast<int>(objectIndex));
            const auto previous = previousWorld.find(object.GetId());
            const glm::mat4 previousModelMatrix = previous != previousWorld.end()
                ? previous->second
                : modelMatrix;
            object.GetMaterial().Apply(
                camera,
                m_lighting,
                m_environmentMap->GetIBL(),
                modelMatrix,
                m_shadowMap.get(),
                object.ReceivesShadow(),
                splitLightingMrt,
                materialDebugMode,
                m_directionalShadowSettings,
                motionFrameState,
                previousModelMatrix);
            object.GetMesh()->Draw();
        }

        drawScope.Success();
        m_renderFrameDiagnostics.rasterRecordCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rasterRecordStart)
                .count();
    }

    if (!usePostProcess && target != nullptr)
    {
        target->Unbind();
    }
}

void SceneRenderer::RenderPostProcessPass(
    const Scene& scene,
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    Framebuffer* target,
    const SceneRenderOptions& options,
    const bool freezeTemporalJitter,
    const bool splitLightingMrt,
    const RenderViewport renderViewport)
{
    {
        SceneRenderTrace::Scope endPassScope("EndScenePass");
        m_screenSpaceEffects->EndScenePass();
        endPassScope.Success();
    }

    const bool pathTracingActive = m_dxrSettings.IsPathTracingActive();
    const bool drawGrid = options.showGrid && scene.GetShowGrid();
    if (drawGrid && !pathTracingActive)
    {
        SceneRenderTrace::Scope gridScope("scene grid pass");
        m_screenSpaceEffects->BeginSceneGridPass();
        m_grid->Draw(camera, splitLightingMrt, false, splitLightingMrt);
        m_screenSpaceEffects->EndSceneGridPass();
        gridScope.Success();
    }

    // PT frame data consumed by RecordDxrPass. UploadEmissiveLights also refreshes the
    // SceneHasTransmission flag that selects Fresnel/refractive NEE shadows instead of the opaque
    // any-hit fast path. These calls were lost during the render-pass refactor.
    if (pathTracingActive && m_dxrAccelerationStructures != nullptr)
    {
        const auto pathTracerFrameDataStart = std::chrono::steady_clock::now();
        if (!m_gpuScene.GetInstances().empty())
        {
            std::vector<glm::mat4> previousWorldMatrices;
            previousWorldMatrices.reserve(m_gpuScene.GetInstances().size());
            for (const GpuSceneInstanceRecord& instance : m_gpuScene.GetInstances())
            {
                previousWorldMatrices.push_back(instance.prevWorld);
            }
            m_dxrAccelerationStructures->UploadPrevInstanceTransforms(
                previousWorldMatrices,
                GfxContext::Get().GetCommandList());
        }
        m_dxrAccelerationStructures->UploadEmissiveLights(
            m_gpuScene,
            GfxContext::Get().GetCommandList());
        m_renderFrameDiagnostics.pathTracerFrameDataCpuMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pathTracerFrameDataStart)
                .count();
    }

    // S1-P4 compatibility is scheduled before RecordDxrPass because PT reference accumulation and
    // ReSTIR can consume history there. ScreenSpaceEffects commits the identity only after Apply
    // draws this viewport.
    m_screenSpaceEffects->SetPtRrBundleMode(m_dxrSettings.GetPtRrBundleMode());
    HistoryRenderProducer historyProducer = HistoryRenderProducer::Raster;
    if (m_dxrSettings.IsEnabled() && GfxContext::Get().IsRaytracingSupported())
    {
        historyProducer = pathTracingActive
            ? HistoryRenderProducer::PathTracer
            : HistoryRenderProducer::Hybrid;
    }
    // Structural/material changes still invalidate reconstruction history. Ordinary instance
    // motion deliberately does not: the PT motion guide and the per-pixel surface validation own
    // temporal rejection, so resetting the entire viewport would make every moving scene noisy.
    const bool transmissionOpticalDomain = pathTracingActive
        && m_dxrAccelerationStructures != nullptr
        && m_dxrAccelerationStructures->SceneHasTransmission();
    const std::uint32_t opticalSceneVersion = transmissionOpticalDomain
        ? m_dxrAccelerationStructures->GetPtSceneVersion()
        : 0u;
    const std::uint32_t opticalMotionVersion = 0u;
    const HistoryCompatibilityTransition historyTransition =
        m_screenSpaceEffects->BeginHistoryCompatibilityFrame(
            historyProducer,
            camera,
            viewportWidth,
            viewportHeight,
            opticalSceneVersion,
            opticalMotionVersion);
    if (historyTransition.Resets(HistoryCompatibilityOwner::RestirTemporal)
        && m_dxrPathTracerDispatch != nullptr)
    {
        m_dxrPathTracerDispatch->InvalidateRestirHistory(
            RenderViewportHistoryId(renderViewport));
    }

    RecordDxrPass(
        scene,
        camera,
        m_screenSpaceEffects->GetRenderWidth(),
        m_screenSpaceEffects->GetRenderHeight(),
        m_screenSpaceEffects->GetSceneDepthSrvCpuHandle(),
        true,
        renderViewport);

    if (target != nullptr)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(target);
    }

    if (drawGrid && m_dxrSettings.IsPathTracingActive())
    {
        m_screenSpaceEffects->SetPathTracerGridOverlayCallback(
            [this](const Camera& cam, const bool useDepthTest) {
                m_grid->Draw(cam, true, true, false, useDepthTest);
            });
    }
    else
    {
        m_screenSpaceEffects->SetPathTracerGridOverlayCallback({});
    }

    {
        SceneRenderTrace::Scope applyScope("ScreenSpaceEffects::Apply");
        const GfxContext::GpuTimerScope gpuScopePost("Post-process");
        m_screenSpaceEffects->Apply(
            camera,
            viewportWidth,
            viewportHeight,
            m_directionalShadowSettings,
            *m_environmentMap);
        applyScope.Success();
    }
    {
        SceneRenderTrace::Scope finalizeAaScope("FinalizeAntiAliasingFrame");
        m_screenSpaceEffects->FinalizeAntiAliasingFrame(camera, freezeTemporalJitter);
        finalizeAaScope.Success();
    }
    {
        SceneRenderTrace::Scope advanceTemporalScope("AdvanceTemporalFrame");
        m_screenSpaceEffects->AdvanceTemporalFrame(camera);
        advanceTemporalScope.Success();
    }

    if (target != nullptr)
    {
        m_screenSpaceEffects->BlitRtDispatchSmokeDebug(target, viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitRtPrimaryDebug(
            target,
            viewportWidth,
            viewportHeight,
            m_dxrSettings.GetMaxTraceDistance());
        m_screenSpaceEffects->BlitRtReflectionDebug(target, viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitRtShadowDebug(target, viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitRtGiDebug(target, viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitRrGuideDebug(target, viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitPtOpticalLayerDebug(target, viewportWidth, viewportHeight);
        // Apply's PT diagnostic views draw directly to the viewport. Do not immediately replace
        // them with the legacy direct PT blit (which made every diagnostic appear as the final
        // noisy PT image regardless of the selected mode).
        if (!m_screenSpaceEffects->PostProcessDebugRenderedThisFrame())
        {
            m_screenSpaceEffects->BlitPathTracer(
                target,
                viewportWidth,
                viewportHeight,
                m_dxrSettings.GetMaxTraceDistance());
        }

        if (drawGrid && m_dxrSettings.IsPathTracingActive()
            && !m_screenSpaceEffects->PathTracerPostIntegratedThisFrame()
            && !m_screenSpaceEffects->PathTracerResolvedViaDlssThisFrame()
            && !m_screenSpaceEffects->PostProcessDebugRenderedThisFrame())
        {
            SceneRenderTrace::Scope gridOverlayScope("grid pt overlay (ldr fallback)");
            bool depthReadOnly = false;
            if (m_screenSpaceEffects->BlitDepthToFramebuffer(target))
            {
                depthReadOnly = target->BindGizmoDrawTarget();
            }
            if (!depthReadOnly)
            {
                target->BindDrawTarget(false);
            }
            m_grid->Draw(camera, false, true, false, depthReadOnly);
            gridOverlayScope.Success();
        }
    }

    if (target != nullptr)
    {
        target->BindDrawTarget(false);
    }

    if (options.showEditorOverlay)
    {
        if (const SceneEditor* editor = scene.TryGetSceneEditor())
        {
            editor->RenderSelectionOverlay(scene, camera, true);
        }
    }
}

void SceneRenderer::RenderGizmoPass(
    const Scene& scene,
    const Camera& camera,
    Framebuffer* target,
    const SceneRenderOptions& options,
    const bool usePostProcess)
{
    const bool drawWorldGizmos =
        options.showCameraGizmos
        || (options.showLightGizmos && scene.GetShowLightGizmos())
        || options.showColliderGizmos;

    if (target == nullptr || !drawWorldGizmos)
    {
        if (target != nullptr)
        {
            target->Unbind();
        }
        return;
    }

    bool viewportDepthReadOnly = false;
    if (usePostProcess)
    {
        if (m_screenSpaceEffects->BlitDepthToFramebuffer(target))
        {
            viewportDepthReadOnly = target->BindGizmoDrawTarget();
        }
        if (!viewportDepthReadOnly)
        {
            target->BindDrawTarget(false);
        }
    }
    else
    {
        viewportDepthReadOnly = target->BindGizmoDrawTarget();
        if (!viewportDepthReadOnly)
        {
            target->BindDrawTarget(false);
        }
    }

    const bool depthReadOnly = viewportDepthReadOnly;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    const SceneSelection& selection = scene.GetSelection();

    if (options.showLightGizmos && scene.GetShowLightGizmos())
    {
        m_lightGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices,
            depthReadOnly);
    }

    if (options.showCameraGizmos)
    {
        m_cameraGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices,
            depthReadOnly);
    }

    if (options.showColliderGizmos)
    {
        m_colliderGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices,
            depthReadOnly);
    }

    if (viewportDepthReadOnly)
    {
        target->BindColorRenderTarget(false, nullptr);
        target->RestoreDepthShaderResource();
    }

    target->Unbind();
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
