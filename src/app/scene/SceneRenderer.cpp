#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/GridRenderer.h"

#include "app/scene/SceneRenderer.h"

#include "app/project/SceneProjectIODetail.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/SceneRenderTrace.h"

#include "app/scene/SceneEditor.h"
#include "engine/camera/Camera.h"
#include "engine/rhi/GfxContext.h"
#include "engine/components/LightComponent.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/Shader.h"
#include "engine/rendering/RenderingPipelineCache.h"
#include "engine/raytracing/DxrTrace.h"

#include <algorithm>
#include <cmath>
#include <limits>

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer() = default;

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
    template<typename Fn>
    void RunGpuInitStep(const char* stepName, Fn&& fn)
    {
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
    self->m_dxrAccelerationStructures.reset();
    self->m_dxrSmokeDispatch.reset();
    self->m_dxrPrimaryDebugDispatch.reset();
    self->m_dxrReflectionsDispatch.reset();
    self->m_dxrShadowsDispatch.reset();
    self->m_dxrGiDispatch.reset();
    self->m_shadowDepthShader.reset();
    self->m_gpuResourcesInitialized = false;
    self->m_gpuResourcesInitInProgress = false;
}

void SceneRenderer::ResetGpuResourcesIfInitFailed() const
{
    if (m_gpuResourcesInitialized || !m_gpuResourcesInitFailed)
    {
        return;
    }

    ResetPartialGpuResources();
    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    self->m_gpuResourcesInitFailed = false;
    self->m_gpuResourcesInitError.clear();
    self->m_gpuResourcesInitInProgress = false;
}

void SceneRenderer::PrepareGpuResourcesForGeometryMsaa(const int msaaSampleCount) const
{
    GfxContext::Get().SetActiveMsaaSampleCount(msaaSampleCount);
    const int activeMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();

    if (m_gpuResourcesInitialized)
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
        self->m_gpuResourcesInitFailed = false;
        self->m_gpuResourcesInitError.clear();
        self->m_gpuResourcesInitInProgress = false;
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

    if (m_gpuResourcesInitialized && m_screenSpaceEffects != nullptr
        && m_screenSpaceEffects->GetMsaaSampleCount() != activeMsaaSampleCount)
    {
        const_cast<ScreenSpaceEffects*>(m_screenSpaceEffects.get())->SetMsaaSampleCount(activeMsaaSampleCount);
    }
}

void SceneRenderer::EnsureGpuResources() const
{
    if (m_gpuResourcesInitialized || m_gpuResourcesInitFailed || m_gpuResourcesInitInProgress)
    {
        return;
    }

    if (!GfxContext::Get().IsInitialized())
    {
        m_gpuResourcesInitFailed = true;
        m_gpuResourcesInitError = "GfxContext is not initialized";
        EngineLog::Error("scene", "GPU init failed: " + m_gpuResourcesInitError);
        return;
    }

    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    self->m_gpuResourcesInitInProgress = true;
    SceneRenderTrace::Scope gpuInitScope("EnsureGpuResources");
    try
    {
        RunGpuInitStep("camera gizmos", [&]() { self->m_cameraGizmos = std::make_unique<CameraGizmoRenderer>(); });
        RunGpuInitStep("grid", [&]() { self->m_grid = std::make_unique<GridRenderer>(); });
        RunGpuInitStep("collider gizmos", [&]() { self->m_colliderGizmos = std::make_unique<ColliderGizmoRenderer>(); });
        RunGpuInitStep("light gizmos", [&]() { self->m_lightGizmos = std::make_unique<LightGizmoRenderer>(); });
        RunGpuInitStep("shadow map", [&]() { self->m_shadowMap = std::make_unique<CascadedShadowMap>(); });
        RunGpuInitStep("environment map", [&]() { self->m_environmentMap = std::make_unique<EnvironmentMap>(); });
        RunGpuInitStep("screen-space effects", [&]() {
            self->m_screenSpaceEffects = std::make_unique<ScreenSpaceEffects>();
            const int geometryMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();
            if (geometryMsaaSampleCount > 1)
            {
                self->m_screenSpaceEffects->SetMsaaSampleCount(geometryMsaaSampleCount);
            }
        });
        RunGpuInitStep("shadow depth shader", [&]() {
            self->m_shadowDepthShader = std::make_unique<Shader>(
                EngineConstants::ShadowDepthVertexShader,
                EngineConstants::ShadowDepthFragmentShader);
        });
        self->m_gpuResourcesInitialized = true;
        self->m_gpuResourcesInitInProgress = false;
        GfxContext::Get().SetMaterialTextureFilterMode(self->m_textureFilterMode);
        GfxContext::Get().SetMaterialTextureAnisotropy(self->m_textureAnisotropy);
        GfxContext::Get().SetMaterialTextureMipBias(self->m_textureMipBias);
        gpuInitScope.Success();
    }
    catch (const std::exception& exception)
    {
        ResetPartialGpuResources();
        self->m_gpuResourcesInitInProgress = false;
        self->m_gpuResourcesInitFailed = true;
        self->m_gpuResourcesInitError = SafeExceptionMessage(exception);
        EngineLog::Error("scene", "GPU init failed: " + self->m_gpuResourcesInitError);
    }
    catch (...)
    {
        ResetPartialGpuResources();
        self->m_gpuResourcesInitInProgress = false;
        self->m_gpuResourcesInitFailed = true;
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
    if (!m_gpuResourcesInitialized)
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
        if (m_dxrSmokeDispatch == nullptr)
        {
            m_dxrSmokeDispatch = std::make_unique<DxrSmokeDispatch>();
        }

        if (m_dxrPrimaryDebugDispatch == nullptr)
        {
            m_dxrPrimaryDebugDispatch = std::make_unique<DxrPrimaryDebugDispatch>();
        }

        if (m_dxrReflectionsDispatch == nullptr)
        {
            m_dxrReflectionsDispatch = std::make_unique<DxrReflectionsDispatch>();
        }

        if (m_dxrShadowsDispatch == nullptr)
        {
            m_dxrShadowsDispatch = std::make_unique<DxrShadowsDispatch>();
        }

        if (m_dxrGiDispatch == nullptr)
        {
            m_dxrGiDispatch = std::make_unique<DxrGiDispatch>();
        }

        const bool smokeReady = m_dxrSmokeDispatch->IsPipelineReady();
        const bool primaryReady = m_dxrPrimaryDebugDispatch->IsPipelineReady();
        const bool reflectionsReady = m_dxrReflectionsDispatch->IsPipelineReady();
        const bool shadowsReady = m_dxrShadowsDispatch->IsPipelineReady();
        const bool giReady = m_dxrGiDispatch->IsPipelineReady();
        if (smokeReady && primaryReady && reflectionsReady && shadowsReady && giReady)
        {
            return;
        }

        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded begin");
        if (!smokeReady)
        {
            m_dxrSmokeDispatch->WarmUpPipelineIfNeeded();
        }
        if (!primaryReady)
        {
            m_dxrPrimaryDebugDispatch->WarmUpPipelineIfNeeded();
        }
        if (!reflectionsReady)
        {
            m_dxrReflectionsDispatch->WarmUpPipelineIfNeeded();
        }
        if (!shadowsReady)
        {
            m_dxrShadowsDispatch->WarmUpPipelineIfNeeded();
        }
        if (!giReady)
        {
            m_dxrGiDispatch->WarmUpPipelineIfNeeded();
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
    if (!m_gpuResourcesInitialized || GfxContext::Get().IsFrameRecording())
    {
        return;
    }

    if (m_environmentMap != nullptr)
    {
        SceneRenderTrace::Scope envScope("SyncGpuResources");
        try
        {
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

    WarmUpDxrPipelineIfNeeded();
}

void SceneRenderer::RecordDxrPass(
    const Scene& scene,
    const Camera& camera,
    const int dispatchWidth,
    const int dispatchHeight,
    const std::uintptr_t depthSrvCpuHandle,
    const bool usePostProcess)
{
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
        }
        return;
    }

    DxrBreadcrumb("render: EnsureScene begin");
    if (m_dxrAccelerationStructures == nullptr)
    {
        m_dxrAccelerationStructures = std::make_unique<DxrAccelerationStructures>();
    }

    m_dxrAccelerationStructures->EnsureScene(
        scene,
        true,
        GfxContext::Get().GetCommandList());
    DxrBreadcrumb("render: EnsureScene end");

    const RenderDebugMode debugMode =
        usePostProcess && m_screenSpaceEffects != nullptr ? m_screenSpaceEffects->GetDebugMode()
                                                          : RenderDebugMode::None;
    const bool smokeDebugMode = debugMode == RenderDebugMode::RtDispatchSmoke;
    const bool primaryDebugViewActive = IsRtPrimaryDebugMode(debugMode);
    const bool primaryTraceEnabled =
        m_dxrSettings.IsDebugTraceEnabled() || primaryDebugViewActive;

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

    // Phase D4 — reflection trace (devdoc/dxr-reflections.md). Requires the scene MRTs, so
    // only runs on the post-process path.
    const bool reflectionDebugViewActive = IsRtReflectionDebugMode(debugMode);
    const bool reflectionsWanted =
        m_dxrSettings.IsReflectionsEnabled() || reflectionDebugViewActive;
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
        frameInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(2);
        frameInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(5);
        frameInputs.directSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(0);
        frameInputs.sunShadowSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(3);
        frameInputs.indirectSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(1);
        frameInputs.prefilterSrvCpuHandle = ibl.GetPrefilterMapSrvCpuHandle();
        frameInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(4);
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
            m_dxrSettings.IsDenoiseEnabled(),
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
    const bool shadowsWanted = m_dxrSettings.IsShadowsEnabled() || shadowDebugViewActive;
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
        shadowInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(2);
        shadowInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(5);
        shadowInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(4);
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

    // Phase D9 — RT diffuse GI trace (devdoc/dxr-diffuse-gi.md). Quality-scaled like reflections;
    // needs the scene MRTs + IBL, so only runs on the post-process path.
    const bool giDebugViewActive = IsRtGiDebugMode(debugMode);
    const bool giWanted = m_dxrSettings.IsGiEnabled() || giDebugViewActive;
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

        const IBL& ibl = m_environmentMap->GetIBL();
        DxrGiDispatch::FrameInputs giInputs{};
        giInputs.depthSrvCpuHandle = depthSrvCpuHandle;
        giInputs.normalSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(2);
        giInputs.material0SrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(5);
        giInputs.directSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(0);
        giInputs.sunShadowSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(3);
        giInputs.indirectSrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(1);
        giInputs.prefilterSrvCpuHandle = ibl.GetPrefilterMapSrvCpuHandle();
        giInputs.velocitySrvCpuHandle = m_screenSpaceEffects->GetSceneColorSrvCpuHandle(4);
        giInputs.environmentIntensity = ibl.GetEnvironmentIntensity();
        giInputs.maxReflectionLod = ibl.GetMaxReflectionLod();

        // In-hit analytic shading inputs (shared hit_shading.hlsli — identical to reflections).
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
        const IrradianceSh9& sh9 = ibl.GetIrradianceSh9();
        for (std::size_t i = 0; i < sh9.coefficients.size() && i < giInputs.irradianceSh9.size(); ++i)
        {
            giInputs.irradianceSh9[i] = sh9.coefficients[i];
        }

        // DispatchRays samples the MRTs from non-pixel shaders; idempotent with the reflection /
        // shadow passes' transitions.
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
            m_dxrSettings.IsGiDenoiseEnabled(),
            m_dxrSettings.GetTemporalBlend(),
            m_dxrSettings.GetReflectionAtrousIterations(),
            m_dxrSettings.IsReflectionAntiFireflyEnabled());
        DxrBreadcrumb("render: gi DispatchIfEnabled end");
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

void SceneRenderer::Render(
    const Scene& scene,
    const Camera& camera,
    int viewportWidth,
    int viewportHeight,
    std::uintptr_t targetFramebuffer,
    const SceneRenderOptions& options)
{
    SceneRenderTrace::Scope renderScope("SceneRenderer::Render");
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized)
    {
        renderScope.Success();
        return;
    }

    GfxContext::Get().ResetDrawSrvTable();

    Framebuffer* target = nullptr;
    if (targetFramebuffer != 0)
    {
        target = reinterpret_cast<Framebuffer*>(targetFramebuffer);
    }

    const bool usePostProcess = m_screenSpaceEffects->IsEnabled();
    const bool freezeTemporalJitter =
        ImGuizmo::IsUsing() || ImGuizmo::IsUsingViewManipulate();

    SceneRenderTrace::Step(
        std::string("render setup postProcess=") + (usePostProcess ? "1" : "0")
        + " shadowPass=" + (options.enableShadowPass ? "1" : "0"));

    if (target != nullptr)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(target);
    }

    {
        SceneRenderTrace::Scope lightingScope("SyncLighting");
        SyncLighting(scene);
        lightingScope.Success();
    }

    if (options.enableShadowPass)
    {
        SceneRenderTrace::Scope shadowScope("RenderShadowPass");
        const GfxContext::GpuTimerScope gpuScopeShadowMaps("Shadow maps");
        RenderShadowPass(scene, camera);
        m_shadowMap->EndFrame();
        shadowScope.Success();
    }

    RenderDebugMode materialDebugMode = RenderDebugMode::None;
    const RenderDebugMode activeDebugMode = m_screenSpaceEffects->GetDebugMode();
    if (IsPbrMaterialDebugMode(activeDebugMode))
    {
        materialDebugMode = activeDebugMode;
    }

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
    }
    else if (target != nullptr)
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
    }

    bool splitLightingMrt = false;
    if (usePostProcess)
    {
        splitLightingMrt = m_screenSpaceEffects->HasSplitLighting();
    }
    else if (target != nullptr)
    {
        splitLightingMrt = target->HasSplitLighting();
    }

    {
        SceneRenderTrace::Scope skyboxScope("RenderSkybox");
        m_environmentMap->RenderSkybox(camera, splitLightingMrt);
        skyboxScope.Success();
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
    if (m_previousWorldMatrices.size() != objects.size())
    {
        m_previousWorldMatrices.resize(objects.size());
        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            m_previousWorldMatrices[objectIndex] =
                scene.GetWorldMatrix(static_cast<int>(objectIndex));
        }
    }

    const MotionVectorFrameState motionFrameState =
        usePostProcess ? m_screenSpaceEffects->GetMotionVectorFrameState() : MotionVectorFrameState{};

    // Spec-IBL handoff: when a reflection composite (RT reflections / SSR) will ADD spec IBL back
    // into the indirect this frame, tell the PBR raster to OMIT it from RT1. This avoids baking the
    // fp16-Inf HDR sun into RT1 only to subtract it back in the composite (which sparkled/blackened
    // at reflected highlights). Kept in lockstep with the composite gating via the shared helper.
    if (m_environmentMap != nullptr)
    {
        // RT reflections require BOTH the master toggle and the reflections sub-toggle; SSR is
        // independent (the helper folds it in). Gating on the master here keeps the omit in
        // lockstep with the composite, which RecordDxrPass disables when the master is off.
        const bool iblReady = m_environmentMap->GetIBL().IsReady();
        const bool dxrReflectionsActive =
            m_dxrSettings.IsEnabled() && m_dxrSettings.IsReflectionsEnabled();
        const bool omitSpecIbl = usePostProcess && m_screenSpaceEffects != nullptr
            && m_screenSpaceEffects->ReflectionCompositeReplacesSpecIbl(
                   dxrReflectionsActive, iblReady, activeDebugMode);
        m_environmentMap->GetIBL().SetReflectionsReplaceSpecIbl(omitSpecIbl);

        // RT diffuse GI replaces the SH diffuse ambient the same way (master && GI enabled). The
        // GI inject re-adds it (traced, or a transient SH recompute when there's no fresh trace).
        const bool dxrGiActive = m_dxrSettings.IsEnabled() && m_dxrSettings.IsGiEnabled();
        const bool omitDiffuseIbl = usePostProcess && m_screenSpaceEffects != nullptr
            && m_screenSpaceEffects->GiInjectReplacesDiffuseIbl(
                   dxrGiActive, iblReady, activeDebugMode);
        m_environmentMap->GetIBL().SetGiReplacesDiffuseIbl(omitDiffuseIbl);
    }

    {
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
                m_previousWorldMatrices[objectIndex]);
            object.GetMesh()->Draw();
        }

        drawScope.Success();
    }

    if (usePostProcess)
    {
        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            m_previousWorldMatrices[objectIndex] =
                scene.GetWorldMatrix(static_cast<int>(objectIndex));
        }
    }
    else if (target != nullptr)
    {
        target->Unbind();
    }

    if (usePostProcess)
    {
        {
            SceneRenderTrace::Scope endPassScope("EndScenePass");
            m_screenSpaceEffects->EndScenePass();
            endPassScope.Success();
        }

        const bool drawGrid = options.showGrid && scene.GetShowGrid();
        if (drawGrid)
        {
            SceneRenderTrace::Scope gridScope("scene grid pass");
            m_screenSpaceEffects->BeginSceneGridPass();
            m_grid->Draw(camera, splitLightingMrt);
            m_screenSpaceEffects->EndSceneGridPass();
            gridScope.Success();
        }

        // D6: the DXR pass records BEFORE Apply so the same-frame RT reflection buffers are
        // ready when Apply's RT specular composite runs. All DXR inputs (scene MRTs, depth)
        // are complete at this point (EndScenePass + grid have run).
        RecordDxrPass(
            scene,
            camera,
            m_screenSpaceEffects->GetRenderWidth(),
            m_screenSpaceEffects->GetRenderHeight(),
            m_screenSpaceEffects->GetSceneDepthSrvCpuHandle(),
            true);

        if (target != nullptr)
        {
            GfxContext::Get().SetBoundOutputFramebuffer(target);
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
            m_screenSpaceEffects->BlitRtDispatchSmokeDebug(
                reinterpret_cast<Framebuffer*>(targetFramebuffer),
                viewportWidth,
                viewportHeight);
            m_screenSpaceEffects->BlitRtPrimaryDebug(
                reinterpret_cast<Framebuffer*>(targetFramebuffer),
                viewportWidth,
                viewportHeight,
                m_dxrSettings.GetMaxTraceDistance());
            m_screenSpaceEffects->BlitRtReflectionDebug(
                reinterpret_cast<Framebuffer*>(targetFramebuffer),
                viewportWidth,
                viewportHeight);
            m_screenSpaceEffects->BlitRtShadowDebug(
                reinterpret_cast<Framebuffer*>(targetFramebuffer),
                viewportWidth,
                viewportHeight);
            m_screenSpaceEffects->BlitRtGiDebug(
                reinterpret_cast<Framebuffer*>(targetFramebuffer),
                viewportWidth,
                viewportHeight);
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
    else if (options.showGrid && scene.GetShowGrid())
    {
        RecordDxrPass(scene, camera, viewportWidth, viewportHeight, 0, false);
        m_grid->Draw(camera, false);
    }
    else if (!usePostProcess)
    {
        RecordDxrPass(scene, camera, viewportWidth, viewportHeight, 0, false);
    }

    if (!usePostProcess && options.showEditorOverlay)
    {
        if (const SceneEditor* editor = scene.TryGetSceneEditor())
        {
            editor->RenderSelectionOverlay(scene, camera, false);
        }
    }

    const bool drawWorldGizmos =
        options.showCameraGizmos
        || (options.showLightGizmos && scene.GetShowLightGizmos())
        || options.showColliderGizmos;

    if (target != nullptr && drawWorldGizmos)
    {
        GfxContext::Get().ResetDrawSrvTable();

        auto* viewportTarget = reinterpret_cast<Framebuffer*>(targetFramebuffer);
        bool viewportDepthReadOnly = false;
        if (usePostProcess)
        {
            if (m_screenSpaceEffects->BlitDepthToFramebuffer(viewportTarget))
            {
                viewportDepthReadOnly = viewportTarget->BindGizmoDrawTarget();
            }
            if (!viewportDepthReadOnly)
            {
                viewportTarget->BindDrawTarget(false);
            }
        }
        else
        {
            viewportDepthReadOnly = viewportTarget->BindGizmoDrawTarget();
            if (!viewportDepthReadOnly)
            {
                viewportTarget->BindDrawTarget(false);
            }
        }

        const bool depthReadOnly = viewportDepthReadOnly;

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
            viewportTarget->BindColorRenderTarget(false, nullptr);
            viewportTarget->RestoreDepthShaderResource();
        }

        viewportTarget->Unbind();
    }
    else if (target != nullptr)
    {
        reinterpret_cast<Framebuffer*>(targetFramebuffer)->Unbind();
    }

    (void)viewportWidth;
    (void)viewportHeight;
    renderScope.Success();
    SceneRenderTrace::CompleteFirstFrame();
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
    if (!m_gpuResourcesInitialized || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return m_environmentMap->GetIBL();
}

const IBL& SceneRenderer::GetIBL() const
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return m_environmentMap->GetIBL();
}

EnvironmentMap& SceneRenderer::GetEnvironmentMap()
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_environmentMap;
}

const EnvironmentMap& SceneRenderer::GetEnvironmentMap() const
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_environmentMap == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_environmentMap;
}

ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects()
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_screenSpaceEffects == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_screenSpaceEffects;
}

const ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects() const
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_screenSpaceEffects == nullptr)
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
    if (!m_gpuResourcesInitialized || m_shadowMap == nullptr)
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

void SceneRenderer::SetTextureMipBias(const float mipBias)
{
    m_textureMipBias = std::clamp(mipBias, -4.0f, 4.0f);
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().SetMaterialTextureMipBias(m_textureMipBias);
    }
}

bool SceneRenderer::ApplyGeometryMsaaReload(
    Scene& scene,
    const int viewportWidth,
    const int viewportHeight,
    std::string* outError)
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_screenSpaceEffects == nullptr)
    {
        if (outError != nullptr)
        {
            *outError = "GPU resources are not initialized.";
        }
        return false;
    }

    if (!m_screenSpaceEffects->IsMsaaPendingReload())
    {
        return true;
    }

    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        if (outError != nullptr)
        {
            *outError = "Viewport size is invalid.";
        }
        return false;
    }

    const int requestedMsaaSampleCount = m_screenSpaceEffects->GetMsaaSampleCount();

    try
    {
        GfxContext::Get().CancelFrame();
        GfxContext::Get().WaitForGpuIdle();
        GfxContext::Get().SetBoundOutputFramebuffer(nullptr);

        GfxContext::Get().SetActiveMsaaSampleCount(requestedMsaaSampleCount);
        RenderingPipelineCache::InvalidateAll();
        scene.InvalidateAllMaterialCachedShaders();
        m_environmentMap->ReloadSkyboxRenderer();
        m_screenSpaceEffects->ReloadGeometryMsaaTargets(viewportWidth, viewportHeight);

        m_gpuResourcesInitFailed = false;
        m_gpuResourcesInitError.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        const std::string message = SafeExceptionMessage(exception);
        EngineLog::Error("scene", "MSAA reload failed: " + message);
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    }
    catch (...)
    {
        EngineLog::Error("scene", "MSAA reload failed: unknown error");
        if (outError != nullptr)
        {
            *outError = "unknown MSAA reload error";
        }
        return false;
    }
}
