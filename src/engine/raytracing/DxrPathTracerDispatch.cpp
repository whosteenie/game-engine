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
    const std::uintptr_t depthSrvCpuHandle,
    const int width,
    const int height,
    const float maxTraceDistance)
{
    m_dispatchedThisFrame = false;

    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled || !pathTracingActive
        || width <= 0 || height <= 0)
    {
        return false;
    }

    if (!accelerationStructures.IsTlasBuilt() || !accelerationStructures.HasGeometryLookup())
    {
        DxrBreadcrumb("path-tracer skipped: TLAS or geometry lookup unavailable");
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

    DxrRootSignature::PrimaryDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.nearPlane = camera.GetNearPlane();
    constants.farPlane = camera.GetFarPlane();
    constants.maxTraceDistance = maxTraceDistance;

    // Reuse the primary-debug dispatch (same root signature + output textures); only the state object
    // and SBT are the path tracer's. The PT shader ignores the bound depth SRV (pure camera rays).
    if (!m_dispatchContext.DispatchPrimaryDebug(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
            accelerationStructures.GetTlasResource(),
            accelerationStructures.GetTlasGpuVirtualAddress(),
            depthSrvCpuHandle,
            accelerationStructures.GetGeometryLookupSrvIndex(),
            accelerationStructures.GetSceneVertexFloatsSrvIndex(),
            accelerationStructures.GetSceneIndicesSrvIndex(),
            width,
            height,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("path tracer dispatch failed: DispatchPrimaryDebug (") + error + ")";
        DxrLogErrorOnce("dispatch-path-tracer-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-path-tracer-failure", failureMessage);
        dispatchScope.Success();
        DxrEnableTrustMode();
        return false;
    }

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

bool DxrPathTracerDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle() != 0
        && m_dispatchContext.GetPrimaryMetadataSrvCpuHandle() != 0;
}
