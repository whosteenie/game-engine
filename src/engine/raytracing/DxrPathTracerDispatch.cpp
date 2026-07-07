#include "engine/raytracing/DxrPathTracerDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstring>

DxrPathTracerDispatch::~DxrPathTracerDispatch()
{
    Release();
}

void DxrPathTracerDispatch::Release()
{
    m_shaderBindingTable.Release();
    m_pipeline.Release();
    m_dispatchContext.Release();
    m_pipelineReady = false;
    m_frameIndex = 0;
}

bool DxrPathTracerDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrPathTracerDispatch::EnsurePipeline(std::string& outError)
{
    outError.clear();
    if (m_pipelineReady)
    {
        return true;
    }

    DxrBreadcrumb("path-tracer EnsurePipeline begin");
    if (!m_pipeline.CreatePathTracerPipeline(outError))
    {
        DxrBreadcrumb("path-tracer EnsurePipeline failed: CreatePathTracerPipeline");
        return false;
    }

    if (!m_shaderBindingTable.BuildPathTracerTable(m_pipeline.GetProperties(), outError))
    {
        DxrBreadcrumb("path-tracer EnsurePipeline failed: BuildPathTracerTable");
        m_pipeline.Release();
        return false;
    }

    m_pipelineReady = true;
    DxrBreadcrumb("path-tracer EnsurePipeline ok");
    return true;
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
    const float maxTraceDistance)
{
    m_dispatchedThisFrame = false;

    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled || !pathTracingActive
        || width <= 0 || height <= 0 || gbufferWidth <= 0 || gbufferHeight <= 0)
    {
        return false;
    }

    if (!accelerationStructures.IsTlasBuilt() || !accelerationStructures.HasGeometryLookup())
    {
        DxrBreadcrumb("path-tracer skipped: TLAS or geometry lookup unavailable");
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

    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        DxrBreadcrumb("path-tracer skipped: CommandList4 unavailable");
        return false;
    }

    std::string error;

    if (!m_pipelineReady)
    {
        if (!EnsurePipeline(error))
        {
            DxrBreadcrumb("path-tracer skipped: pipeline not ready");
            return false;
        }
    }

    DxrBreadcrumb("path-tracer dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-path-tracer");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();

    DxrRootSignature::ReflectionDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    constants.gbufferWidth = static_cast<std::uint32_t>(gbufferWidth);
    constants.gbufferHeight = static_cast<std::uint32_t>(gbufferHeight);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    std::memcpy(constants.worldToView, glm::value_ptr(viewMatrix), sizeof(constants.worldToView));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.environmentIntensity = frameInputs.environmentIntensity;
    constants.maxReflectionLod = frameInputs.maxReflectionLod;
    constants.frameIndex = m_frameIndex;
    constants.samplesPerPixel = 4; // P2: max path bounces (tunable in P5)
    constants.sunDirection[0] = frameInputs.sunDirection.x;
    constants.sunDirection[1] = frameInputs.sunDirection.y;
    constants.sunDirection[2] = frameInputs.sunDirection.z;
    constants.sunIntensity = frameInputs.sunIntensity;
    constants.sunColor[0] = frameInputs.sunColor.x;
    constants.sunColor[1] = frameInputs.sunColor.y;
    constants.sunColor[2] = frameInputs.sunColor.z;

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

bool DxrPathTracerDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle() != 0
        && m_dispatchContext.GetPrimaryMetadataSrvCpuHandle() != 0;
}
