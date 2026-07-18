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

