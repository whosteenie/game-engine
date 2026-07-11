#include "engine/raytracing/DxrPathTracerDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrRestirDispatch.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>

DxrPathTracerDispatch::~DxrPathTracerDispatch()
{
    Release();
}

void DxrPathTracerDispatch::Release()
{
    ReleaseCore();
    m_frameIndex = 0;
}

bool DxrPathTracerDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrPathTracerDispatch::EnsurePipeline(std::string& outError)
{
    return EnsurePipelineWith(
        "path-tracer",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreatePathTracerPipeline(pipelineError);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildPathTracerTable(pipeline.GetProperties(), tableError);
        },
        outError);
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
    const float ptBloomHaloIntensity,
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

    if (!m_pipelineReady && !EnsurePipeline(error))
    {
        DxrBreadcrumb("path-tracer skipped: pipeline not ready");
        return false;
    }

    DxrBreadcrumb("path-tracer dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-path-tracer");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 unjitteredViewProj = unjitteredProjection * viewMatrix;
    // Real-time / DLSS: primary rays through pixel-center unjittered frustum so the traced hit,
    // unjittered MVs, and jittered HW depth describe one consistent surface point per pixel.
    const glm::mat4 invViewProj = frameInputs.centerPrimaryRays
        ? glm::inverse(unjitteredViewProj)
        : glm::inverse(viewProj);
    const glm::mat4 prevViewProj = frameInputs.motionHistoryValid
        ? frameInputs.prevViewProjection
        : unjitteredViewProj;
    const glm::mat4 prevInvViewProj = glm::inverse(prevViewProj);
    const glm::vec3 cameraPos = camera.GetPosition();
    const glm::vec3 prevCameraPos = frameInputs.motionHistoryValid
        ? frameInputs.prevCameraPos
        : cameraPos;

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
        2.0f * std::tan(glm::radians(camera.GetFov()) * 0.5f)
        / static_cast<float>(std::max(height, 1));
    // _PadUnjitteredViewProj.w = motion history valid (lit.vs uTemporalHistoryValid parity).
    constants.paddingUnjitteredViewProj[3] =
        frameInputs.motionHistoryValid ? 1.0f : 0.0f;
    constants.emissiveLightCount = accelerationStructures.GetEmissiveLightCount();
    constants.emissiveLightPickWeightSum = accelerationStructures.GetEmissiveLightPickWeightSum();
    constants.ptBloomHaloIntensity = std::max(ptBloomHaloIntensity, 0.0f);
    constants.ptDebugIsolateMode =
        static_cast<float>(std::clamp(ptDebugIsolateMode, 0, 9));
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
    constants.envLightImportanceInvWeightSum =
        frameInputs.envImportanceWeightSum > 0.0f
            ? 1.0f / frameInputs.envImportanceWeightSum
            : 0.0f;

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
    dispatchInputs.envImportanceCdfSrvIndex = frameInputs.envImportanceCdfSrvIndex;
    dispatchInputs.envEquirectSrvCpuHandle = frameInputs.envEquirectSrvCpuHandle;

    if (!m_dispatchContext.DispatchPathTracer(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
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
    return true;
}

bool DxrPathTracerDispatch::DispatchRestirTemporal(
    DxrRestirDispatch& restirDispatch,
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    void* commandList,
    const float maxTraceDistance,
    const std::uint32_t sceneVersion,
    const bool realTimeMode,
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

    m_dispatchContext.InvalidateRestirHistoryIfSceneChanged(sceneVersion);

    const glm::mat4 viewProj = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();

    DxrRootSignature::RestirTemporalConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferWidth());
    constants.outputHeight = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferHeight());
    constants.historyValid = 1u;
    constants.frameIndex = m_frameIndex;
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.shadeOutput = shadeOutput ? 1u : 0u;
    constants.spatialSampleCount = 5u;
    constants.spatialRadius = 10.0f;
    constants.spatialIteration = 0u;

    std::string error;
    const GfxContext::GpuTimerScope gpuScope("Path tracer/ReSTIR temporal");
    if (!m_dispatchContext.DispatchRestirTemporal(
            commandList4,
            restirDispatch.GetStateObject(),
            restirDispatch.GetGlobalRootSignature(),
            restirDispatch.GetTemporalShaderBindingTable(),
            accelerationStructures.GetTlasResource(),
            accelerationStructures.GetTlasGpuVirtualAddress(),
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

    const glm::mat4 viewProj = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();

    // Two iterations, radius 20 → 10 (restir-pt.md §3).
    constexpr std::uint32_t kSpatialIterations = 2u;
    constexpr float kSpatialRadii[kSpatialIterations] = {10.0f, 5.0f};

    for (std::uint32_t iteration = 0; iteration < kSpatialIterations; ++iteration)
    {
        DxrRootSignature::RestirTemporalConstants constants{};
        constants.outputWidth = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferWidth());
        constants.outputHeight = static_cast<std::uint32_t>(m_dispatchContext.GetRestirBufferHeight());
        constants.historyValid = 1u;
        constants.frameIndex = m_frameIndex;
        std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
        constants.cameraPos[0] = cameraPos.x;
        constants.cameraPos[1] = cameraPos.y;
        constants.cameraPos[2] = cameraPos.z;
        constants.maxTraceDistance = maxTraceDistance;
        constants.shadeOutput = shadeOutput ? 1u : 0u;
        constants.spatialSampleCount = 5u;
        constants.spatialRadius = kSpatialRadii[iteration];
        constants.spatialIteration = iteration;

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
                constants,
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
