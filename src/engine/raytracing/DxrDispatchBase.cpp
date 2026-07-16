#include "engine/raytracing/DxrDispatchBase.h"

#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

void DxrDispatchBase::ReleaseCore()
{
    m_shaderBindingTable.Release();
    m_pipeline.Release();
    m_dispatchContext.Release();
    m_pipelineReady = false;
}

void DxrDispatchBase::ResetDispatchResources()
{
    // Output textures, descriptors, reservoirs, and temporal history are project/viewport state.
    // The RTPSO and SBT contain no scene pointers and remain valid for the lifetime of the device.
    m_dispatchContext.Release();
}

bool DxrDispatchBase::EnsurePipelineWith(
    const char* const traceLabel,
    CreatePipelineFn createPipeline,
    BuildShaderTableFn buildShaderTable,
    std::string& outError)
{
    outError.clear();
    if (m_pipelineReady)
    {
        return true;
    }

    DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline begin");
    if (!createPipeline(m_pipeline, outError))
    {
        DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline failed: create pipeline");
        return false;
    }

    if (!buildShaderTable(m_shaderBindingTable, m_pipeline, outError))
    {
        DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline failed: build SBT");
        m_pipeline.Release();
        return false;
    }

    m_pipelineReady = true;
    DxrBreadcrumb(std::string(traceLabel) + " EnsurePipeline ok");
    return true;
}

ID3D12GraphicsCommandList4* DxrDispatchBase::ResolveDispatchCommandList(
    const DxrAccelerationStructures& accelerationStructures,
    const bool dxrEnabled,
    const int width,
    const int height,
    void* const commandList,
    const DxrDispatchGeometryRequirement geometryRequirement,
    const char* const skipBreadcrumbLabel) const
{
    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    if (!accelerationStructures.IsTlasBuilt())
    {
        DxrBreadcrumb(std::string(skipBreadcrumbLabel) + " skipped: TLAS not built");
        return nullptr;
    }

    if (geometryRequirement == DxrDispatchGeometryRequirement::TlasAndGeometryLookup
        && !accelerationStructures.HasGeometryLookup())
    {
        DxrBreadcrumb(std::string(skipBreadcrumbLabel) + " skipped: geometry lookup unavailable");
        return nullptr;
    }

    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        DxrBreadcrumb(std::string(skipBreadcrumbLabel) + " skipped: CommandList4 unavailable");
        return nullptr;
    }

    return commandList4;
}
