#include "app/scene/SceneRenderer.h"

#include <glad/glad.h>

#include "app/scene/SceneEditor.h"
#include "engine/camera/Camera.h"
#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/GridRenderer.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/components/LightComponent.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/Shader.h"

#include <algorithm>
#include <limits>

SceneRenderer::SceneRenderer()
    : m_cameraGizmos(std::make_unique<CameraGizmoRenderer>()),
      m_grid(std::make_unique<GridRenderer>()),
      m_colliderGizmos(std::make_unique<ColliderGizmoRenderer>()),
      m_lightGizmos(std::make_unique<LightGizmoRenderer>()),
      m_shadowMap(std::make_unique<CascadedShadowMap>()),
      m_ibl(std::make_unique<IBL>(EngineConstants::EnvironmentHdr)),
      m_screenSpaceEffects(std::make_unique<ScreenSpaceEffects>()),
      m_shadowDepthShader(std::make_unique<Shader>(
          EngineConstants::ShadowDepthVertexShader,
          EngineConstants::ShadowDepthFragmentShader))
{
}

SceneRenderer::~SceneRenderer() = default;

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

            const GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
            if (object.GetMaterial().IsDoubleSided())
            {
                glDisable(GL_CULL_FACE);
            }

            m_shadowDepthShader->SetMat4("uModel", scene.GetWorldMatrix(static_cast<int>(objectIndex)));
            object.GetMesh()->Draw();

            if (object.GetMaterial().IsDoubleSided() && cullFaceEnabled)
            {
                glEnable(GL_CULL_FACE);
            }
        }
    }
}

void SceneRenderer::Render(
    const Scene& scene,
    const Camera& camera,
    int viewportWidth,
    int viewportHeight,
    unsigned int targetFramebuffer,
    const SceneRenderOptions& options)
{
    GLint previousFramebuffer = 0;
    const bool renderToTarget = targetFramebuffer != 0;
    if (renderToTarget)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
        glViewport(0, 0, viewportWidth, viewportHeight);
        glClearColor(0.08f, 0.09f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    SyncLighting(scene);

    RenderShadowPass(scene, camera);
    m_shadowMap->EndFrame();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, viewportWidth, viewportHeight);

    const bool usePostProcess = m_screenSpaceEffects->IsEnabled();

    RenderDebugMode materialDebugMode = RenderDebugMode::None;
    const RenderDebugMode activeDebugMode = m_screenSpaceEffects->GetDebugMode();
    if (activeDebugMode >= RenderDebugMode::ShadowFactor &&
        activeDebugMode <= RenderDebugMode::CascadeIndex)
    {
        materialDebugMode = activeDebugMode;
    }

    if (usePostProcess)
    {
        m_screenSpaceEffects->Resize(viewportWidth, viewportHeight);
        m_screenSpaceEffects->BeginScenePass();
    }
    else if (renderToTarget)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
        glViewport(0, 0, viewportWidth, viewportHeight);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);

    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        const glm::mat4 modelMatrix = scene.GetWorldMatrix(static_cast<int>(objectIndex));
        const GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
        if (object.GetMaterial().IsDoubleSided())
        {
            glDisable(GL_CULL_FACE);
        }

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

        if (object.GetMaterial().IsDoubleSided() && cullFaceEnabled)
        {
            glEnable(GL_CULL_FACE);
        }
    }

    if (usePostProcess)
    {
        if (options.showGrid && scene.GetShowGrid())
        {
            m_grid->Draw(camera, true);
        }

        m_screenSpaceEffects->EndScenePass();

        if (renderToTarget)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
        }

        m_screenSpaceEffects->Apply(camera, viewportWidth, viewportHeight, m_directionalShadowSettings);
        m_screenSpaceEffects->BlitDepthToFramebuffer(
            renderToTarget ? targetFramebuffer : 0,
            viewportWidth,
            viewportHeight);

        if (renderToTarget)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
            glViewport(0, 0, viewportWidth, viewportHeight);
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

    if (renderToTarget)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
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
    return *m_ibl;
}

const IBL& SceneRenderer::GetIBL() const
{
    return *m_ibl;
}

ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects()
{
    return *m_screenSpaceEffects;
}

const ScreenSpaceEffects& SceneRenderer::GetScreenSpaceEffects() const
{
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
    return *m_shadowMap;
}
