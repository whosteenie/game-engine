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
#include "engine/rendering/Shader.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/platform/ExceptionMessage.h"

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

        ShaderCache::Clear();
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
            ShaderCache::Clear();
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
        RunGpuInitStep("screen-space effects", [&]() { self->m_screenSpaceEffects = std::make_unique<ScreenSpaceEffects>(); });
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
        SceneRenderTrace::Scope envScope("SyncGpuResources");
        m_environmentMap->SyncGpuResources();
        envScope.Success();
    }

    {
        SceneRenderTrace::Scope lightingScope("SyncLighting");
        SyncLighting(scene);
        lightingScope.Success();
    }

    if (options.enableShadowPass)
    {
        SceneRenderTrace::Scope shadowScope("RenderShadowPass");
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

    {
        SceneRenderTrace::Scope drawScope("draw scene objects");
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

        if (target != nullptr)
        {
            GfxContext::Get().SetBoundOutputFramebuffer(target);
        }

        {
            SceneRenderTrace::Scope applyScope("ScreenSpaceEffects::Apply");
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
        m_grid->Draw(camera, false);
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
        ShaderCache::Clear();
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
