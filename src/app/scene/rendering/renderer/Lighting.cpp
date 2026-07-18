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

