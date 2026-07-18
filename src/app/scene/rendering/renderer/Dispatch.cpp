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

