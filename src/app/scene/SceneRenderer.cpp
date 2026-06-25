#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/GridRenderer.h"

#include "app/scene/SceneRenderer.h"

#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"

#include "app/scene/SceneEditor.h"
#include "engine/camera/Camera.h"
#include "engine/rhi/GfxContext.h"
#include "engine/components/LightComponent.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/Shader.h"
#include "engine/platform/ExceptionMessage.h"

#include <algorithm>
#include <limits>

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::ThrowGpuResourcesUnavailable() const
{
    throw std::runtime_error(
        m_gpuResourcesInitError.empty()
            ? std::string("GPU resources are not initialized.")
            : m_gpuResourcesInitError);
}

void SceneRenderer::EnsureGpuResources() const
{
    if (m_gpuResourcesInitialized)
    {
        return;
    }

    if (m_gpuResourcesInitFailed)
    {
        return;
    }

    SceneRenderer* self = const_cast<SceneRenderer*>(this);
    try
    {
        self->m_cameraGizmos = std::make_unique<CameraGizmoRenderer>();
        self->m_grid = std::make_unique<GridRenderer>();
        self->m_colliderGizmos = std::make_unique<ColliderGizmoRenderer>();
        self->m_lightGizmos = std::make_unique<LightGizmoRenderer>();
        self->m_shadowMap = std::make_unique<CascadedShadowMap>();
        self->m_ibl = std::make_unique<IBL>(EngineConstants::EnvironmentHdr);
        self->m_screenSpaceEffects = std::make_unique<ScreenSpaceEffects>();
        self->m_shadowDepthShader = std::make_unique<Shader>(
            EngineConstants::ShadowDepthVertexShader,
            EngineConstants::ShadowDepthFragmentShader);
        self->m_gpuResourcesInitialized = true;
    }
    catch (const std::exception& exception)
    {
        self->m_gpuResourcesInitFailed = true;
        self->m_gpuResourcesInitError = SafeExceptionMessage(exception);
        EngineLog::Error("scene", "GPU init failed: " + self->m_gpuResourcesInitError);
    }
    catch (...)
    {
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

        m_shadowDepthShader->Use();
        m_shadowDepthShader->SetMat4(
            "uLightSpaceMatrix",
            m_shadowMap->GetLightSpaceMatrix(cascadeIndex));

        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            const SceneObject& object = objects[objectIndex];
            if (!object.IsRenderable() || !object.CastsShadow())
            {
                continue;
            }

            m_shadowDepthShader->SetMat4("uModel", scene.GetWorldMatrix(static_cast<int>(objectIndex)));
            m_shadowDepthShader->FlushUniforms();
            object.GetMesh()->Draw();
        }
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
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized)
    {
        return;
    }

    Framebuffer* target = nullptr;
    if (targetFramebuffer != 0)
    {
        target = reinterpret_cast<Framebuffer*>(targetFramebuffer);
    }

    const bool usePostProcess = m_screenSpaceEffects->IsEnabled();

    if (target != nullptr && !usePostProcess)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(target);
    }

    SyncLighting(scene);
    if (options.enableShadowPass)
    {
        RenderShadowPass(scene, camera);
        m_shadowMap->EndFrame();
    }

    RenderDebugMode materialDebugMode = RenderDebugMode::None;
    const RenderDebugMode activeDebugMode = m_screenSpaceEffects->GetDebugMode();
    if (IsPbrMaterialDebugMode(activeDebugMode))
    {
        materialDebugMode = activeDebugMode;
    }

    if (usePostProcess)
    {
        m_screenSpaceEffects->Resize(viewportWidth, viewportHeight);
        m_screenSpaceEffects->BeginScenePass();
    }
    else if (target != nullptr)
    {
        target->Bind();
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
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
            *m_ibl,
            modelMatrix,
            m_shadowMap.get(),
            object.ReceivesShadow(),
            usePostProcess,
            materialDebugMode,
            m_directionalShadowSettings);
        object.GetMesh()->Draw();
    }

    if (usePostProcess)
    {
        if (options.showGrid && scene.GetShowGrid())
        {
            m_grid->Draw(camera, true);
        }

        m_screenSpaceEffects->EndScenePass();
        if (target != nullptr)
        {
            GfxContext::Get().SetBoundOutputFramebuffer(target);
        }
        m_screenSpaceEffects->Apply(camera, viewportWidth, viewportHeight, m_directionalShadowSettings);
        m_screenSpaceEffects->BlitDepthToFramebuffer(
            targetFramebuffer,
            viewportWidth,
            viewportHeight);

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

    if (target != nullptr && !usePostProcess)
    {
        target->BindDrawTarget(false);
    }

    const SceneSelection& selection = scene.GetSelection();
    if (options.showCameraGizmos)
    {
        m_cameraGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices);
    }

    if (options.showLightGizmos && scene.GetShowLightGizmos())
    {
        m_lightGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices);
    }

    if (options.showColliderGizmos)
    {
        m_colliderGizmos->Draw(
            camera,
            objects,
            [&scene](int objectIndex) { return scene.GetWorldMatrix(objectIndex); },
            selection.indices);
    }

    if (!usePostProcess && options.showEditorOverlay)
    {
        if (const SceneEditor* editor = scene.TryGetSceneEditor())
        {
            editor->RenderSelectionOverlay(scene, camera, false);
        }
    }

    if (target != nullptr)
    {
        target->Unbind();
    }

    (void)viewportWidth;
    (void)viewportHeight;
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
    if (!m_gpuResourcesInitialized || m_ibl == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_ibl;
}

const IBL& SceneRenderer::GetIBL() const
{
    EnsureGpuResources();
    if (!m_gpuResourcesInitialized || m_ibl == nullptr)
    {
        ThrowGpuResourcesUnavailable();
    }

    return *m_ibl;
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
