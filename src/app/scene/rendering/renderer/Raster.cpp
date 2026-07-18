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

