#include "app/scene/rendering/SceneRenderer.h"
#include "engine/camera/Camera.h"
#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
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

