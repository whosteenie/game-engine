#include "engine/raytracing/DxrPathTracerDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrRestirDispatch.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace
{
    const char* GetSerPolicyLabel(const DxrPathTracerDispatch::SerOverride value)
    {
        switch (value)
        {
        case DxrPathTracerDispatch::SerOverride::ForceOff: return "force_off";
        case DxrPathTracerDispatch::SerOverride::ForceOn: return "force_on";
        case DxrPathTracerDispatch::SerOverride::Automatic: return "automatic";
        }
        return "missing";
    }

    TemporalCameraState CameraStateFromCamera(const Camera& camera)
    {
        const glm::mat4 view = camera.GetViewMatrix();
        const glm::mat4 projection = camera.GetUnjitteredProjectionMatrix();
        return TemporalCamera::MakeState(
            view,
            projection,
            glm::inverse(projection * view),
            camera.GetPosition(),
            camera.GetProjectionJitter());
    }
}

DxrPathTracerDispatch::~DxrPathTracerDispatch()
{
    Release();
}

void DxrPathTracerDispatch::Release()
{
    ReleaseCore();
    m_diagnosticShaderBindingTable.Release();
    m_diagnosticPipeline.Release();
    m_serShaderBindingTable.Release();
    m_serPipeline.Release();
    m_serDiagnosticShaderBindingTable.Release();
    m_serDiagnosticPipeline.Release();
    m_diagnosticPipelineReady = false;
    m_serPipelineReady = false;
    m_serDiagnosticPipelineReady = false;
    m_activeDiagnosticPermutation = false;
    m_activeSerPermutation = false;
    m_frameIndex = 0;
    m_lastCameraPacket = {};
    m_lastCameraConstants = {};
}

void DxrPathTracerDispatch::SetSerOverride(const SerOverride value)
{
    m_serOverride = value;
    GfxContext::Get().SetDxrRuntimeSerPolicy(GetSerPolicyLabel(value));
}

bool DxrPathTracerDispatch::IsPipelineReady() const
{
    if (!DxrDispatchBase::IsPipelineReady() || !m_diagnosticPipelineReady)
    {
        return false;
    }
    return !GfxContext::Get().IsShaderExecutionReorderingSupported()
        || (m_serPipelineReady && m_serDiagnosticPipelineReady);
}

bool DxrPathTracerDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    // Build both permutations during the existing DXR warmup window. Switching debug views then
    // selects already-created state objects instead of compiling in an interactive frame.
    if (!EnsurePipeline(false, false, error) || !EnsurePipeline(true, false, error))
    {
        return false;
    }
    if (!GfxContext::Get().IsShaderExecutionReorderingSupported())
    {
        return true;
    }
    return EnsurePipeline(false, true, error) && EnsurePipeline(true, true, error);
}

bool DxrPathTracerDispatch::EnsurePipeline(
    const bool diagnosticPermutation,
    const bool serPermutation,
    std::string& outError)
{
    DxrPipeline* pipeline = nullptr;
    ShaderBindingTable* shaderBindingTable = nullptr;
    bool* pipelineReady = nullptr;
    const char* traceLabel = nullptr;
    if (serPermutation)
    {
        pipeline = diagnosticPermutation ? &m_serDiagnosticPipeline : &m_serPipeline;
        shaderBindingTable = diagnosticPermutation ? &m_serDiagnosticShaderBindingTable : &m_serShaderBindingTable;
        pipelineReady = diagnosticPermutation ? &m_serDiagnosticPipelineReady : &m_serPipelineReady;
        traceLabel = diagnosticPermutation ? "path-tracer-ser-diagnostic" : "path-tracer-ser";
    }

    if (serPermutation)
    {
        outError.clear();
        if (*pipelineReady)
        {
            return true;
        }
        DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline begin");
        if (!pipeline->CreatePathTracerPipeline(outError, diagnosticPermutation, true))
        {
            const DxrPipeline::PathTracerPipelineStatus& status = pipeline->GetPathTracerPipelineStatus();
            GfxContext::Get().ReportDxrPathTracerPipelineResult(
                diagnosticPermutation, true, status.compilerLibrary, status.rtpso, outError.c_str());
            DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline failed: create pipeline");
            return false;
        }
        if (!shaderBindingTable->BuildPathTracerTable(pipeline->GetProperties(), outError))
        {
            const DxrPipeline::PathTracerPipelineStatus& status = pipeline->GetPathTracerPipelineStatus();
            GfxContext::Get().ReportDxrPathTracerPipelineResult(
                diagnosticPermutation, true, status.compilerLibrary, status.rtpso, outError.c_str());
            DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline failed: build SBT");
            pipeline->Release();
            return false;
        }
        const DxrPipeline::PathTracerPipelineStatus& status = pipeline->GetPathTracerPipelineStatus();
        GfxContext::Get().ReportDxrPathTracerPipelineResult(
            diagnosticPermutation, true, status.compilerLibrary, status.rtpso, "none");
        *pipelineReady = true;
        DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline ok");
        return true;
    }

    if (diagnosticPermutation)
    {
        outError.clear();
        if (m_diagnosticPipelineReady)
        {
            return true;
        }

        DxrBreadcrumb("path-tracer-diagnostic EnsurePipeline begin");
        if (!m_diagnosticPipeline.CreatePathTracerPipeline(outError, true, false))
        {
            const DxrPipeline::PathTracerPipelineStatus& status = m_diagnosticPipeline.GetPathTracerPipelineStatus();
            GfxContext::Get().ReportDxrPathTracerPipelineResult(
                true, false, status.compilerLibrary, status.rtpso, outError.c_str());
            DxrBreadcrumb("path-tracer-diagnostic EnsurePipeline failed: create pipeline");
            return false;
        }
        if (!m_diagnosticShaderBindingTable.BuildPathTracerTable(
                m_diagnosticPipeline.GetProperties(), outError))
        {
            const DxrPipeline::PathTracerPipelineStatus& status = m_diagnosticPipeline.GetPathTracerPipelineStatus();
            GfxContext::Get().ReportDxrPathTracerPipelineResult(
                true, false, status.compilerLibrary, status.rtpso, outError.c_str());
            DxrBreadcrumb("path-tracer-diagnostic EnsurePipeline failed: build SBT");
            m_diagnosticPipeline.Release();
            return false;
        }

        const DxrPipeline::PathTracerPipelineStatus& status = m_diagnosticPipeline.GetPathTracerPipelineStatus();
        GfxContext::Get().ReportDxrPathTracerPipelineResult(
            true, false, status.compilerLibrary, status.rtpso, "none");
        m_diagnosticPipelineReady = true;
        DxrBreadcrumb("path-tracer-diagnostic EnsurePipeline ok");
        return true;
    }

    const bool basePipelineReady = EnsurePipelineWith(
        "path-tracer",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreatePathTracerPipeline(pipelineError, false, false);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildPathTracerTable(pipeline.GetProperties(), tableError);
        },
        outError);
    const DxrPipeline::PathTracerPipelineStatus& status = m_pipeline.GetPathTracerPipelineStatus();
    GfxContext::Get().ReportDxrPathTracerPipelineResult(
        false, false, status.compilerLibrary, status.rtpso, basePipelineReady ? "none" : outError.c_str());
    return basePipelineReady;
}

bool DxrPathTracerDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    const bool dxrEnabled,
    const bool pathTracingActive,
    void* commandList,
    const FrameInputs& frameInputs,
    const int width,
    const int height,
    const int gbufferWidth,
    const int gbufferHeight,
    const float maxTraceDistance,
    const int ptMaxBounces,
    const bool ptRussianRoulette,
    const bool ptFireflyClamp,
    const float ptAmbientStrength,
    const int ptAmbientAoRayCount,
    const int ptDebugIsolateMode)
{
    m_dispatchedThisFrame = false;

    if (!pathTracingActive)
    {
        return false;
    }

    if (gbufferWidth <= 0 || gbufferHeight <= 0)
    {
        return false;
    }

    if (frameInputs.depthSrvCpuHandle == 0 || frameInputs.normalSrvCpuHandle == 0
        || frameInputs.material0SrvCpuHandle == 0 || frameInputs.directSrvCpuHandle == 0
        || frameInputs.sunShadowSrvCpuHandle == 0 || frameInputs.indirectSrvCpuHandle == 0
        || frameInputs.prefilterSrvCpuHandle == 0 || frameInputs.velocitySrvCpuHandle == 0
        || frameInputs.materialSrvIndex == UINT32_MAX)
    {
        DxrBreadcrumb("path-tracer skipped: frame inputs unavailable");
        return false;
    }

    ID3D12GraphicsCommandList4* commandList4 = ResolveDispatchCommandList(
        accelerationStructures,
        dxrEnabled,
        width,
        height,
        commandList,
        DxrDispatchGeometryRequirement::TlasAndGeometryLookup,
        "path-tracer");
    if (commandList4 == nullptr)
    {
        return false;
    }

    std::string error;

    const bool diagnosticPermutation = ptDebugIsolateMode != 0;
    const bool serPermutation = ShouldUseSerPermutation(
        GfxContext::Get().IsShaderExecutionReorderingSupported(), m_serOverride);
    const char* const fallbackReason = serPermutation
        ? "none"
        : (m_serOverride == SerOverride::ForceOff
            ? "requested_force_off"
            : "capability_gate_not_supported");
    GfxContext::Get().SetDxrRuntimeSerPolicy(GetSerPolicyLabel(m_serOverride));
    GfxContext::Get().ReportDxrPathTracerSelection(
        diagnosticPermutation, serPermutation, fallbackReason);
    const bool selectedPipelineReady = serPermutation
        ? (diagnosticPermutation ? m_serDiagnosticPipelineReady : m_serPipelineReady)
        : (diagnosticPermutation ? m_diagnosticPipelineReady : m_pipelineReady);
    if (!selectedPipelineReady)
    {
        if (!EnsurePipeline(diagnosticPermutation, serPermutation, error))
        {
            DxrBreadcrumb("path-tracer skipped: pipeline not ready");
            return false;
        }
    }

    if (m_activeDiagnosticPermutation != diagnosticPermutation || m_activeSerPermutation != serPermutation)
    {
        // Reservoir contents and the displayed PT output must never cross a permutation boundary.
        // This is intentionally the only invalidation performed for a switch.
        m_dispatchContext.InvalidateRestirHistory();
        m_activeDiagnosticPermutation = diagnosticPermutation;
        m_activeSerPermutation = serPermutation;
    }

    DxrBreadcrumb("path-tracer dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-path-tracer");

    // FrameInputs owns the values below. Camera remains a retained call-site duplicate only; it
    // must describe the same current camera and never supplies a temporal fallback.
    const TemporalCameraState duplicateCurrent = CameraStateFromCamera(camera);
    const bool duplicateCameraAgrees =
        TemporalCamera::Agree(frameInputs.cameraPacket.current, duplicateCurrent);
    assert(duplicateCameraAgrees && "DxrPathTracerDispatch current camera packet mismatch");
    if (!duplicateCameraAgrees
        || !TryBuildCameraConstants(
            frameInputs.cameraPacket,
            frameInputs.centerPrimaryRays,
            m_lastCameraConstants))
    {
        m_dispatchContext.InvalidateRestirHistory();
        DxrBreadcrumb("path-tracer skipped: current camera packet incomplete");
        return false;
    }
    if (!m_lastCameraConstants.historyValid)
    {
        // These are complete current-camera values, never identity/zero substitutes. Both the PT
        // motion flag and ReSTIR history flag prohibit temporal consumers from reading them.
        m_dispatchContext.InvalidateRestirHistory();
    }
    m_lastCameraPacket = frameInputs.cameraPacket;

    const glm::mat4& viewMatrix = m_lastCameraConstants.view;
    const glm::mat4& viewProj = m_lastCameraConstants.viewProjection;
    const glm::mat4& invViewProj = m_lastCameraConstants.inverseViewProjection;
    const glm::mat4& unjitteredViewProj =
        m_lastCameraConstants.unjitteredViewProjection;
    // g_PrevViewProj (MV output projection + sky anchor): prev UNJITTERED — jitter must never
    // appear in the emitted motion vectors (motionVectorsJittered = false).
    const glm::mat4& prevViewProj = m_lastCameraConstants.previousViewProjection;
    // g_PrevInvViewProj (glass virtual-motion ray REPLAY only): prev VIEW composed with the
    // CURRENT jittered projection, so the replayed prev primary ray enters the pane at the same
    // sub-pixel offset as this frame's ray. Jitter then cancels out of the virtual MV: static
    // camera -> replay ray == current ray -> refracted background points coincide -> glass MV is
    // exactly 0; camera motion survives untouched. Deriving this from the prev UNJITTERED matrix
    // (pre-cab2529 behavior) diverged the two refraction paths by the per-frame Halton delta,
    // which refraction amplifies -> frame-varying MVs on STATIC glass -> RR boiled the glass.
    const glm::mat4& prevInvViewProj =
        m_lastCameraConstants.previousReplayInverseViewProjection;
    const glm::vec3& cameraPos = m_lastCameraConstants.worldPosition;
    const glm::vec3& prevCameraPos = m_lastCameraConstants.previousWorldPosition;

    DxrRootSignature::ReflectionDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    constants.gbufferWidth = static_cast<std::uint32_t>(gbufferWidth);
    constants.gbufferHeight = static_cast<std::uint32_t>(gbufferHeight);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    std::memcpy(constants.worldToView, glm::value_ptr(viewMatrix), sizeof(constants.worldToView));
    std::memcpy(
        constants.unjitteredViewProj,
        glm::value_ptr(unjitteredViewProj),
        sizeof(constants.unjitteredViewProj));
    std::memcpy(constants.prevViewProj, glm::value_ptr(prevViewProj), sizeof(constants.prevViewProj));
    std::memcpy(
        constants.prevInvViewProj,
        glm::value_ptr(prevInvViewProj),
        sizeof(constants.prevInvViewProj));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.prevCameraPos[0] = prevCameraPos.x;
    constants.prevCameraPos[1] = prevCameraPos.y;
    constants.prevCameraPos[2] = prevCameraPos.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.environmentIntensity = frameInputs.environmentIntensity;
    constants.maxReflectionLod = frameInputs.maxReflectionLod;
    constants.frameIndex = m_frameIndex;
    constants.samplesPerPixel = static_cast<std::uint32_t>(std::clamp(ptMaxBounces, 1, 16));
    // Path-tracer-only packing in reflection fields unused by this pass (see path_tracer.hlsl).
    constants.aoRayCount = ptFireflyClamp ? 1u : 0u;
    constants.hasGiTrace = ptRussianRoulette ? 1u : 0u;
    constants.sunDirection[0] = frameInputs.sunDirection.x;
    constants.sunDirection[1] = frameInputs.sunDirection.y;
    constants.sunDirection[2] = frameInputs.sunDirection.z;
    constants.sunIntensity = frameInputs.sunIntensity;
    constants.sunColor[0] = frameInputs.sunColor.x;
    constants.sunColor[1] = frameInputs.sunColor.y;
    constants.sunColor[2] = frameInputs.sunColor.z;
    // Path-tracer-only: g_RoughnessCutoff > 0.5 => pixel-center primary rays (real-time + DLSS).
    constants.roughnessCutoff = frameInputs.centerPrimaryRays ? 1.0f : 0.0f;
    // Path-tracer-only: g_GiStrength / _PadUnjitteredViewProj.x repurposed for ambient v2
    // (devdoc/dxr/pt/crevice-darkening.md). g_SunAngularTanRadius = soft sun disk (devdoc/dxr/pt/soft-sun.md).
    constants.giStrength = std::clamp(ptAmbientStrength, 0.0f, 2.0f);
    constants.paddingUnjitteredViewProj[0] =
        static_cast<float>(std::clamp(ptAmbientAoRayCount, 0, 8));
    // P4b: _PadUnjitteredViewProj.y = per-instance prev-transform buffer valid this frame
    // (object+camera motion in the PT MV output; else camera-only reprojection fallback).
    const std::uint32_t prevTransformsSrvIndex =
        accelerationStructures.GetPrevInstanceTransformsSrvIndex();
    constants.paddingUnjitteredViewProj[1] =
        prevTransformsSrvIndex != UINT32_MAX ? 1.0f : 0.0f;
    // _PadUnjitteredViewProj.z = ray-cone pixel spread angle (radians/pixel) for albedo LOD.
    constants.paddingUnjitteredViewProj[2] =
        2.0f / std::abs(frameInputs.cameraPacket.current.projection[1][1])
        / static_cast<float>(std::max(height, 1));
    // _PadUnjitteredViewProj.w = motion history valid (lit.vs uTemporalHistoryValid parity).
    constants.paddingUnjitteredViewProj[3] =
        m_lastCameraConstants.historyValid ? 1.0f : 0.0f;
    constants.emissiveLightCount = accelerationStructures.GetEmissiveLightCount();
    constants.emissiveLightPickWeightSum = accelerationStructures.GetEmissiveLightPickWeightSum();
    constants.ptDebugIsolateMode =
        static_cast<float>(std::clamp(ptDebugIsolateMode, 0, kPtDebugIsolateModeMax));
    constants.sunAngularTanRadius = std::tan(
        glm::radians(std::clamp(frameInputs.sunAngularRadiusDegrees, 0.0f, 5.0f)));
    // Opaque-fast NEE shadows when the scene has no dielectrics (a0cc7f8 regression fix).
    constants.sceneHasTransmission =
        accelerationStructures.SceneHasTransmission() ? 1.0f : 0.0f;
    // L2 SH diffuse sky irradiance — AO-gated at the primary hit in real-time mode.
    for (std::size_t i = 0; i < frameInputs.irradianceSh9.size(); ++i)
    {
        constants.irradianceSh9[i][0] = frameInputs.irradianceSh9[i].x;
        constants.irradianceSh9[i][1] = frameInputs.irradianceSh9[i].y;
        constants.irradianceSh9[i][2] = frameInputs.irradianceSh9[i].z;
        constants.irradianceSh9[i][3] = frameInputs.irradianceSh9[i].w;
    }
    constants.envLightImportanceCount = frameInputs.envImportanceSampleCount;
    constants.envIsCdfWidth = frameInputs.envImportanceCdfWidth;
    constants.envIsCdfHeight = frameInputs.envImportanceCdfHeight;
    constants.envDirectLightingLuminanceClamp = frameInputs.envDirectLightingLuminanceClamp;
    constants.restirDiCandidateCount = static_cast<float>(frameInputs.restirDiCandidateCount);
    constants.restirGiInitialEnabled = frameInputs.restirGiInitialEnabled ? 1.0f : 0.0f;
    constants._restirDiPad1 = frameInputs.environmentRotationYRadians;

    m_lastEnvEquirectSrvCpuHandle = frameInputs.envEquirectSrvCpuHandle;
    m_lastEnvImportanceCdfSrvIndex = frameInputs.envImportanceCdfSrvIndex;
    m_lastEnvImportanceCount = frameInputs.envImportanceSampleCount;
    m_lastEnvCdfWidth = frameInputs.envImportanceCdfWidth;
    m_lastEnvCdfHeight = frameInputs.envImportanceCdfHeight;
    m_lastEnvironmentIntensity = frameInputs.environmentIntensity;
    m_lastEnvironmentRotationYRadians = frameInputs.environmentRotationYRadians;
    m_lastEnvDirectLuminanceClamp = frameInputs.envDirectLightingLuminanceClamp;
    m_lastSunIntensity = frameInputs.sunIntensity;
    m_lastSunDirection = frameInputs.sunDirection;
    m_lastSunAngularTanRadius = std::tan(glm::radians(frameInputs.sunAngularRadiusDegrees));
    m_lastDebugMode = static_cast<std::uint32_t>(std::max(ptDebugIsolateMode, 0));

    DxrDispatchContext::ReflectionDispatchInputs dispatchInputs{};
    dispatchInputs.tlasResource = accelerationStructures.GetTlasResource();
    dispatchInputs.tlasGpuVirtualAddress = accelerationStructures.GetTlasGpuVirtualAddress();
    dispatchInputs.depthSrvCpuHandle = frameInputs.depthSrvCpuHandle;
    dispatchInputs.normalSrvCpuHandle = frameInputs.normalSrvCpuHandle;
    dispatchInputs.material0SrvCpuHandle = frameInputs.material0SrvCpuHandle;
    dispatchInputs.geometryLookupSrvIndex = accelerationStructures.GetGeometryLookupSrvIndex();
    dispatchInputs.sceneVertexFloatsSrvIndex = accelerationStructures.GetSceneVertexFloatsSrvIndex();
    dispatchInputs.sceneIndicesSrvIndex = accelerationStructures.GetSceneIndicesSrvIndex();
    dispatchInputs.materialSrvIndex = frameInputs.materialSrvIndex;
    dispatchInputs.directSrvCpuHandle = frameInputs.directSrvCpuHandle;
    dispatchInputs.sunShadowSrvCpuHandle = frameInputs.sunShadowSrvCpuHandle;
    dispatchInputs.indirectSrvCpuHandle = frameInputs.indirectSrvCpuHandle;
    dispatchInputs.prefilterSrvCpuHandle = frameInputs.prefilterSrvCpuHandle;
    dispatchInputs.velocitySrvCpuHandle = frameInputs.velocitySrvCpuHandle;
    dispatchInputs.prevInstanceTransformsSrvIndex = prevTransformsSrvIndex;
    dispatchInputs.emissiveLightsSrvIndex = accelerationStructures.GetEmissiveLightsSrvIndex();
    dispatchInputs.emissiveTrianglesSrvIndex = accelerationStructures.GetEmissiveTrianglesSrvIndex();
    dispatchInputs.emissiveLightAliasSrvIndex = accelerationStructures.GetEmissiveLightAliasSrvIndex();
    dispatchInputs.emissiveTriangleAliasSrvIndex = accelerationStructures.GetEmissiveTriangleAliasSrvIndex();
    dispatchInputs.emissiveLightByInstanceSrvIndex = accelerationStructures.GetEmissiveLightByInstanceSrvIndex();
    dispatchInputs.envImportanceCdfSrvIndex = frameInputs.envImportanceCdfSrvIndex;
    dispatchInputs.envEquirectSrvCpuHandle = frameInputs.envEquirectSrvCpuHandle;

    const GfxContext::GpuTimerScope gpuScopePrimary("Path tracer/Primary rays");
    if (!m_dispatchContext.DispatchPathTracer(
            commandList4,
            serPermutation
                ? (diagnosticPermutation ? m_serDiagnosticPipeline.GetStateObject() : m_serPipeline.GetStateObject())
                : (diagnosticPermutation ? m_diagnosticPipeline.GetStateObject() : m_pipeline.GetStateObject()),
            serPermutation
                ? (diagnosticPermutation ? m_serDiagnosticPipeline.GetGlobalRootSignature() : m_serPipeline.GetGlobalRootSignature())
                : (diagnosticPermutation ? m_diagnosticPipeline.GetGlobalRootSignature() : m_pipeline.GetGlobalRootSignature()),
            serPermutation
                ? (diagnosticPermutation ? m_serDiagnosticShaderBindingTable : m_serShaderBindingTable)
                : (diagnosticPermutation ? m_diagnosticShaderBindingTable : m_shaderBindingTable),
            dispatchInputs,
            width,
            height,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("path tracer dispatch failed: DispatchPathTracer (") + error + ")";
        DxrLogErrorOnce("dispatch-path-tracer-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-path-tracer-failure", failureMessage);
        dispatchScope.Success();
        DxrEnableTrustMode();
        return false;
    }

    ++m_frameIndex;
    DxrBreadcrumb("path-tracer dispatch ok");
    dispatchScope.Success();
    DxrEnableTrustMode();
    m_dispatchedThisFrame = true;
    GfxContext::Get().ReportDxrPathTracerDispatch(diagnosticPermutation, serPermutation);
    return true;
}

bool DxrPathTracerDispatch::DispatchRestirTemporal(
    DxrRestirDispatch& restirDispatch,
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    void* commandList,
    const float maxTraceDistance,
    const std::uint32_t sceneVersion,
    const std::uint32_t motionVersion,
    const bool realTimeMode,
    const bool enableDiTemporal,
    const bool enableGiTemporal,
    const bool shadeOutput)
{
    if (!realTimeMode || !m_dispatchedThisFrame || !restirDispatch.IsPipelineReady())
    {
        return false;
    }

    auto* commandList4 = static_cast<ID3D12GraphicsCommandList4*>(commandList);
    if (commandList4 == nullptr)
    {
        return false;
    }

    m_dispatchContext.InvalidateRestirHistoryIfSceneChanged(sceneVersion, motionVersion);

    const bool duplicateCameraAgrees = TemporalCamera::Agree(
        m_lastCameraPacket.current,
        CameraStateFromCamera(camera));
    assert(duplicateCameraAgrees && "ReSTIR temporal current camera packet mismatch");
    if (!duplicateCameraAgrees)
    {
        m_dispatchContext.InvalidateRestirHistory();
        return false;
    }

    DxrRootSignature::RestirTemporalConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferWidth());
    constants.outputHeight = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferHeight());
    constants.historyValid = m_lastCameraConstants.historyValid ? 1u : 0u;
    constants.frameIndex = m_frameIndex;
    std::memcpy(
        constants.invViewProj,
        glm::value_ptr(m_lastCameraConstants.inverseViewProjection),
        sizeof(constants.invViewProj));
    constants.cameraPos[0] = m_lastCameraConstants.worldPosition.x;
    constants.cameraPos[1] = m_lastCameraConstants.worldPosition.y;
    constants.cameraPos[2] = m_lastCameraConstants.worldPosition.z;
    constants.prevCameraPos[0] = m_lastCameraConstants.previousWorldPosition.x;
    constants.prevCameraPos[1] = m_lastCameraConstants.previousWorldPosition.y;
    constants.prevCameraPos[2] = m_lastCameraConstants.previousWorldPosition.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.shadeOutput = shadeOutput ? 1u : 0u;
    constants.spatialSampleCount = 5u;
    constants.spatialRadius = 10.0f;
    constants.spatialIteration = 0u;
    constants.emissiveLightCount = accelerationStructures.GetEmissiveLightCount();
    constants.emissiveLightPickWeightSum = accelerationStructures.GetEmissiveLightPickWeightSum();
    constants.envImportanceCount = m_lastEnvImportanceCount;
    constants.envCdfWidth = m_lastEnvCdfWidth;
    constants.envCdfHeight = m_lastEnvCdfHeight;
    constants.environmentIntensity = m_lastEnvironmentIntensity;
    constants.environmentRotationYRadians = m_lastEnvironmentRotationYRadians;
    constants.envDirectLuminanceClamp = m_lastEnvDirectLuminanceClamp;
    constants.analyticSunActive = m_lastSunIntensity > 1e-4f ? 1.0f : 0.0f;
    constants.sunDirection[0] = m_lastSunDirection.x;
    constants.sunDirection[1] = m_lastSunDirection.y;
    constants.sunDirection[2] = m_lastSunDirection.z;
    constants.sunAngularTanRadius = m_lastSunAngularTanRadius;
    constants.debugMode = m_lastDebugMode;
    constants.enableDiTemporal = enableDiTemporal ? 1u : 0u;
    constants.enableGiTemporal = enableGiTemporal ? 1u : 0u;

    std::string error;
    const GfxContext::GpuTimerScope gpuScope("Path tracer/ReSTIR temporal");
    if (!m_dispatchContext.DispatchRestirTemporal(
            commandList4,
            restirDispatch.GetStateObject(),
            restirDispatch.GetGlobalRootSignature(),
            restirDispatch.GetTemporalShaderBindingTable(),
            accelerationStructures.GetTlasResource(),
            accelerationStructures.GetTlasGpuVirtualAddress(),
            accelerationStructures.GetEmissiveLightsSrvIndex() != UINT32_MAX
                ? accelerationStructures.GetEmissiveLightsSrvIndex()
                : accelerationStructures.GetGeometryLookupSrvIndex(),
            accelerationStructures.GetEmissiveTrianglesSrvIndex() != UINT32_MAX
                ? accelerationStructures.GetEmissiveTrianglesSrvIndex()
                : accelerationStructures.GetGeometryLookupSrvIndex(),
            m_lastEnvImportanceCdfSrvIndex != UINT32_MAX
                ? m_lastEnvImportanceCdfSrvIndex
                : accelerationStructures.GetGeometryLookupSrvIndex(),
            m_lastEnvEquirectSrvCpuHandle,
            constants,
            error))
    {
        DxrLogErrorOnce(
            "dispatch-restir-temporal-failure",
            std::string("ReSTIR temporal failed: ") + error);
        return false;
    }

    return true;
}

bool DxrPathTracerDispatch::DispatchRestirSpatial(
    DxrRestirDispatch& restirDispatch,
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    void* commandList,
    const float maxTraceDistance,
    const bool realTimeMode,
    const bool enableDiSpatial,
    const bool enableGiBoilingFilter,
    const bool enableGiSpatialReuse,
    const bool shadeOutput)
{
    if (!realTimeMode || !m_dispatchedThisFrame || !restirDispatch.IsSpatialPipelineReady())
    {
        return false;
    }

    auto* commandList4 = static_cast<ID3D12GraphicsCommandList4*>(commandList);
    if (commandList4 == nullptr)
    {
        return false;
    }

    const bool duplicateCameraAgrees = TemporalCamera::Agree(
        m_lastCameraPacket.current,
        CameraStateFromCamera(camera));
    assert(duplicateCameraAgrees && "ReSTIR spatial current camera packet mismatch");
    if (!duplicateCameraAgrees)
    {
        m_dispatchContext.InvalidateRestirHistory();
        return false;
    }

    // RTXDI DI performs one spatial resampling pass over temporal reservoirs. Feeding spatially
    // resampled reservoirs through another pass recursively correlates neighborhoods and produces
    // large persistent blotches at low initial-candidate counts.
    constexpr std::uint32_t kSpatialIterations = 1u;
    constexpr float kSpatialRadii[kSpatialIterations] = {10.0f};

    const auto makeSpatialConstants = [&](const std::uint32_t iteration, const bool shadeThisPass) {
        DxrRootSignature::RestirTemporalConstants constants{};
        constants.outputWidth = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferWidth());
        constants.outputHeight = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferHeight());
        constants.historyValid = m_lastCameraConstants.historyValid ? 1u : 0u;
        constants.frameIndex = m_frameIndex;
        std::memcpy(
            constants.invViewProj,
            glm::value_ptr(m_lastCameraConstants.inverseViewProjection),
            sizeof(constants.invViewProj));
        constants.cameraPos[0] = m_lastCameraConstants.worldPosition.x;
        constants.cameraPos[1] = m_lastCameraConstants.worldPosition.y;
        constants.cameraPos[2] = m_lastCameraConstants.worldPosition.z;
        constants.prevCameraPos[0] = m_lastCameraConstants.previousWorldPosition.x;
        constants.prevCameraPos[1] = m_lastCameraConstants.previousWorldPosition.y;
        constants.prevCameraPos[2] = m_lastCameraConstants.previousWorldPosition.z;
        constants.maxTraceDistance = maxTraceDistance;
        constants.shadeOutput = shadeThisPass ? 1u : 0u;
        constants.spatialSampleCount = 5u;
        constants.spatialRadius = kSpatialRadii[iteration];
        constants.spatialIteration = iteration;
        constants.emissiveLightCount = accelerationStructures.GetEmissiveLightCount();
        constants.emissiveLightPickWeightSum = accelerationStructures.GetEmissiveLightPickWeightSum();
        constants.envImportanceCount = m_lastEnvImportanceCount;
        constants.envCdfWidth = m_lastEnvCdfWidth;
        constants.envCdfHeight = m_lastEnvCdfHeight;
        constants.environmentIntensity = m_lastEnvironmentIntensity;
        constants.environmentRotationYRadians = m_lastEnvironmentRotationYRadians;
        constants.envDirectLuminanceClamp = m_lastEnvDirectLuminanceClamp;
        constants.analyticSunActive = m_lastSunIntensity > 1e-4f ? 1.0f : 0.0f;
        constants.sunDirection[0] = m_lastSunDirection.x;
        constants.sunDirection[1] = m_lastSunDirection.y;
        constants.sunDirection[2] = m_lastSunDirection.z;
        constants.sunAngularTanRadius = m_lastSunAngularTanRadius;
        constants.debugMode = m_lastDebugMode;
        // The shared cbuffer fields select the corresponding resampling domain in either pass.
        constants.enableDiTemporal = enableDiSpatial ? 1u : 0u;
        constants.enableGiTemporal = enableGiSpatialReuse ? 1u : 0u;
        return constants;
    };

    if (enableGiBoilingFilter)
    {
        const DxrRootSignature::RestirTemporalConstants filterConstants =
            makeSpatialConstants(0u, false);
        std::string filterError;
        const GfxContext::GpuTimerScope gpuScope("Path tracer/ReSTIR GI boiling filter");
        if (!m_dispatchContext.DispatchRestirSpatial(
                commandList4,
                restirDispatch.GetStateObject(),
                restirDispatch.GetGlobalRootSignature(),
                restirDispatch.GetGiBoilingFilterShaderBindingTable(),
                accelerationStructures.GetTlasResource(),
                accelerationStructures.GetTlasGpuVirtualAddress(),
                accelerationStructures.GetEmissiveLightsSrvIndex() != UINT32_MAX
                    ? accelerationStructures.GetEmissiveLightsSrvIndex()
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                accelerationStructures.GetEmissiveTrianglesSrvIndex() != UINT32_MAX
                    ? accelerationStructures.GetEmissiveTrianglesSrvIndex()
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                m_lastEnvImportanceCdfSrvIndex != UINT32_MAX
                    ? m_lastEnvImportanceCdfSrvIndex
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                m_lastEnvEquirectSrvCpuHandle,
                filterConstants,
                true,
                filterError))
        {
            DxrLogErrorOnce(
                "dispatch-restir-gi-boiling-filter-failure",
                std::string("ReSTIR GI boiling filter failed: ") + filterError);
            return false;
        }
    }

    for (std::uint32_t iteration = 0; iteration < kSpatialIterations; ++iteration)
    {
        const DxrRootSignature::RestirTemporalConstants constants = makeSpatialConstants(
            iteration,
            shadeOutput && iteration + 1u == kSpatialIterations);

        std::string error;
        const GfxContext::GpuTimerScope gpuScope(
            iteration == 0u ? "Path tracer/ReSTIR spatial 0" : "Path tracer/ReSTIR spatial 1");
        if (!m_dispatchContext.DispatchRestirSpatial(
                commandList4,
                restirDispatch.GetStateObject(),
                restirDispatch.GetGlobalRootSignature(),
                restirDispatch.GetSpatialShaderBindingTable(),
                accelerationStructures.GetTlasResource(),
                accelerationStructures.GetTlasGpuVirtualAddress(),
                accelerationStructures.GetEmissiveLightsSrvIndex() != UINT32_MAX
                    ? accelerationStructures.GetEmissiveLightsSrvIndex()
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                accelerationStructures.GetEmissiveTrianglesSrvIndex() != UINT32_MAX
                    ? accelerationStructures.GetEmissiveTrianglesSrvIndex()
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                m_lastEnvImportanceCdfSrvIndex != UINT32_MAX
                    ? m_lastEnvImportanceCdfSrvIndex
                    : accelerationStructures.GetGeometryLookupSrvIndex(),
                m_lastEnvEquirectSrvCpuHandle,
                constants,
                false,
                error))
        {
            DxrLogErrorOnce(
                "dispatch-restir-spatial-failure",
                std::string("ReSTIR spatial failed: ") + error);
            return false;
        }
    }

    return true;
}

void DxrPathTracerDispatch::FinalizePathTracerSurfaceHistory(void* commandList)
{
    m_dispatchContext.FinalizePathTracerSurfaceHistory(
        static_cast<ID3D12GraphicsCommandList*>(commandList));
}

std::uintptr_t DxrPathTracerDispatch::GetPrimaryOutputSrvCpuHandle() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle();
}

std::uintptr_t DxrPathTracerDispatch::GetPrimaryMetadataSrvCpuHandle() const
{
    return m_dispatchContext.GetPrimaryMetadataSrvCpuHandle();
}

ID3D12Resource* DxrPathTracerDispatch::GetPrimaryOutputResource() const
{
    return m_dispatchContext.GetPrimaryOutputResource();
}

std::uint32_t DxrPathTracerDispatch::GetPrimaryOutputResourceState() const
{
    return m_dispatchContext.GetPrimaryOutputResourceState();
}

void DxrPathTracerDispatch::SetPrimaryOutputResourceState(const std::uint32_t state)
{
    m_dispatchContext.SetPrimaryOutputResourceState(state);
}

ID3D12Resource* DxrPathTracerDispatch::GetPathTracerDepthResource() const
{
    return m_dispatchContext.GetPathTracerDepthResource();
}

std::uint32_t DxrPathTracerDispatch::GetPathTracerDepthResourceState() const
{
    return m_dispatchContext.GetPathTracerDepthResourceState();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerDepthSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerDepthSrvCpuHandle();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerMotionSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerMotionSrvCpuHandle();
}

ID3D12Resource* DxrPathTracerDispatch::GetPathTracerMotionResource() const
{
    return m_dispatchContext.GetPathTracerMotionResource();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerDiffuseAlbedoSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerDiffuseAlbedoSrvCpuHandle();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerSpecularAlbedoSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerSpecularAlbedoSrvCpuHandle();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerNormalRoughnessSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerNormalRoughnessSrvCpuHandle();
}

bool DxrPathTracerDispatch::IsPathTracerPrevSurfaceHistoryValid() const
{
    return m_dispatchContext.IsPathTracerPrevSurfaceHistoryValid();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerPrevDepthSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerPrevDepthSrvCpuHandle();
}

std::uintptr_t DxrPathTracerDispatch::GetPathTracerPrevNormalRoughnessSrvCpuHandle() const
{
    return m_dispatchContext.GetPathTracerPrevNormalRoughnessSrvCpuHandle();
}

bool DxrPathTracerDispatch::HasRestirBuffers() const
{
    return m_dispatchContext.HasRestirBuffers();
}

std::uint32_t DxrPathTracerDispatch::GetPathTracerMotionResourceState() const
{
    return m_dispatchContext.GetPathTracerMotionResourceState();
}

bool DxrPathTracerDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle() != 0
        && m_dispatchContext.GetPrimaryMetadataSrvCpuHandle() != 0;
}
