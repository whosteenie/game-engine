#include "engine/raytracing/dispatch/DxrDispatchContext.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"

#include "engine/raytracing/core/DxrContext.h"
#include "engine/raytracing/core/DxrGpuResource.h"
#include "engine/raytracing/pipeline/DxrPipeline.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/raytracing/restir/RestirTypes.h"
#include "engine/raytracing/pipeline/ShaderBindingTable.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/HresultFormat.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstdlib>
#include <cstring>

#include "engine/raytracing/dispatch/context/Detail.h"

void DxrDispatchContext::ReleaseRetiredOutputs()
{
    for (RetiredOutput& retired : m_retiredOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredOutputs.clear();
}

DxrDispatchContext::~DxrDispatchContext()
{
    Release();
}

void DxrDispatchContext::Release()
{
    ReleaseRetiredOutputs();
    DestroyOutputResource(m_outputResource, m_outputAllocation, m_outputSrvIndex, m_outputUavIndex);
    m_outputSrvCpuHandle = 0;
    m_outputWidth = 0;
    m_outputHeight = 0;
    m_outputResourceState = 0;

    ReleaseRetiredPrimaryOutputs();
    for (RetiredOutput& retired : m_retiredPrimaryMetadata)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }
    m_retiredPrimaryMetadata.clear();

    DestroyOutputResource(
        m_primaryOutputResource,
        m_primaryOutputAllocation,
        m_primaryOutputSrvIndex,
        m_primaryOutputUavIndex);
    DestroyOutputResource(
        m_primaryMetadataResource,
        m_primaryMetadataAllocation,
        m_primaryMetadataSrvIndex,
        m_primaryMetadataUavIndex);
    m_primaryOutputSrvCpuHandle = 0;
    m_primaryMetadataSrvCpuHandle = 0;
    m_primaryOutputWidth = 0;
    m_primaryOutputHeight = 0;
    m_primaryOutputResourceState = 0;
    m_primaryMetadataResourceState = 0;

    RetireOrDestroyReflectionTexture(m_ptDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionOutputTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptDirectTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfacePositionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfaceMaterialTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfaceAlbedoMetallicTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfacePositionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceMaterialTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceAlbedoMetallicTexture);
    m_ptPrevSurfaceHistoryValid = false;

    ReleaseRetiredRestirBuffers();
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
    RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[1]);
    m_restirBufferWidth = 0;
    m_restirBufferHeight = 0;
    m_restirElementCount = 0;
    m_restirWriteIndex = 0;
    m_restirReservoirHistoryValid = false;

    ReleaseRetiredReflectionOutputs();
    for (ReflectionTexture& texture : m_reflectionTextures)
    {
        DestroyOutputResource(texture.resource, texture.allocation, texture.srvIndex, texture.uavIndex);
        texture.srvCpuHandle = 0;
        texture.state = 0;
    }
    m_reflectionOutputSrvCpuHandle = 0;
    m_reflectionDenoisedSrvCpuHandle = 0;
    m_reflectionOutputWidth = 0;
    m_reflectionOutputHeight = 0;
    m_reflectionDispatchWidth = 0;
    m_reflectionDispatchHeight = 0;

    ReleaseRetiredShadowOutputs();
    for (ReflectionTexture& texture : m_shadowTextures)
    {
        DestroyOutputResource(texture.resource, texture.allocation, texture.srvIndex, texture.uavIndex);
        texture.srvCpuHandle = 0;
        texture.state = 0;
    }
    m_shadowPenumbraSrvCpuHandle = 0;
    m_shadowDenoisedSrvCpuHandle = 0;
    m_shadowOutputWidth = 0;
    m_shadowOutputHeight = 0;
    m_shadowDispatchWidth = 0;
    m_shadowDispatchHeight = 0;

    ReleaseRetiredGiOutputs();
    for (ReflectionTexture& texture : m_giTextures)
    {
        DestroyOutputResource(texture.resource, texture.allocation, texture.srvIndex, texture.uavIndex);
        texture.srvCpuHandle = 0;
        texture.state = 0;
    }
    m_giOutputSrvCpuHandle = 0;
    m_giDenoisedSrvCpuHandle = 0;
    m_giOutputWidth = 0;
    m_giOutputHeight = 0;
    m_giDispatchWidth = 0;
    m_giDispatchHeight = 0;

    if (m_tlasSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(m_tlasSrvIndex);
        m_tlasSrvIndex = UINT32_MAX;
    }

    for (const std::uint32_t index : m_retiredTlasSrvIndices)
    {
        GfxContext::Get().FreeOffscreenSrv(index);
    }
    m_retiredTlasSrvIndices.clear();
    m_tlasSrvResource = nullptr;
    m_tlasSrvGpuVirtualAddress = 0;

}

void DxrDispatchContext::ReleaseRetiredReflectionOutputs()
{
    for (RetiredOutput& retired : m_retiredReflectionOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredReflectionOutputs.clear();
}

void DxrDispatchContext::ReleaseRetiredShadowOutputs()
{
    for (RetiredOutput& retired : m_retiredShadowOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredShadowOutputs.clear();
}

void DxrDispatchContext::ReleaseRetiredGiOutputs()
{
    for (RetiredOutput& retired : m_retiredGiOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredGiOutputs.clear();
}

void DxrDispatchContext::ReleaseRetiredRestirBuffers()
{
    for (RetiredOutput& retired : m_retiredRestirBuffers)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredRestirBuffers.clear();
}

bool DxrDispatchContext::EnsureOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid dispatch output dimensions";
        return false;
    }

    // Previous frame's retired outputs are safe to free once BeginFrame waited on the fence.
    ReleaseRetiredOutputs();

    if (m_outputResource != nullptr && m_outputWidth == width && m_outputHeight == height)
    {
        return true;
    }

    if (m_outputResource != nullptr && GfxContext::Get().IsFrameRecording())
    {
        // Same command list may already reference the current output (e.g. scene view then game view).
        if (m_outputWidth >= width && m_outputHeight >= height)
        {
            return true;
        }

        RetiredOutput retired{};
        retired.resource = m_outputResource;
        retired.allocation = m_outputAllocation;
        retired.srvIndex = m_outputSrvIndex;
        retired.uavIndex = m_outputUavIndex;
        m_retiredOutputs.push_back(retired);

        m_outputResource = nullptr;
        m_outputAllocation = nullptr;
        m_outputSrvIndex = UINT32_MAX;
        m_outputUavIndex = UINT32_MAX;
        m_outputSrvCpuHandle = 0;
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_outputResourceState = 0;
    }
    else if (m_outputResource != nullptr)
    {
        DestroyOutputResource(m_outputResource, m_outputAllocation, m_outputSrvIndex, m_outputUavIndex);
        m_outputSrvCpuHandle = 0;
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_outputResourceState = 0;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR output texture";
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    const HRESULT createResult = allocator->CreateResource(
        &allocationDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        &m_outputAllocation,
        IID_PPV_ARGS(&m_outputResource));
    if (FAILED(createResult))
    {
        outError = "failed to allocate DXR dispatch output texture ("
            + std::to_string(width) + "x" + std::to_string(height)
            + ", HRESULT=" + HresultFormat::Format(createResult) + ")";
        return false;
    }

    m_outputWidth = width;
    m_outputHeight = height;
    m_outputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    m_outputSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_outputUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (m_outputSrvIndex == UINT32_MAX || m_outputUavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR output descriptors";
        DestroyOutputResource(m_outputResource, m_outputAllocation, m_outputSrvIndex, m_outputUavIndex);
        return false;
    }

    CreateOutputDescriptors();
    return true;
}

void DxrDispatchContext::CreateOutputDescriptors()
{
    if (m_outputResource == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_outputSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    GfxContext::Get().CreateShaderResourceView(m_outputResource, &srvDesc, m_outputSrvIndex);
    m_outputSrvCpuHandle = srvHandle.ptr;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    GfxContext::Get().CreateUnorderedAccessView(m_outputResource, nullptr, &uavDesc, m_outputUavIndex);
}

bool DxrDispatchContext::CreateTlasSrv(
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    std::string& outError)
{
    outError.clear();
    if (tlasResource == nullptr || tlasGpuVirtualAddress == 0)
    {
        outError = "TLAS resource unavailable for DXR dispatch";
        return false;
    }

    if (m_tlasSrvIndex == UINT32_MAX)
    {
        m_tlasSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (m_tlasSrvIndex == UINT32_MAX)
        {
            outError = "failed to allocate TLAS SRV descriptor";
            const std::string gpuError = GfxContext::Get().GetLastGpuAllocationError();
            if (!gpuError.empty())
            {
                outError += " (" + gpuError + ")";
            }
            return false;
        }
    }

    // Descriptor tables are static while a dispatch is executing. All DXR passes in a frame use
    // the same TLAS, so recreating this descriptor for temporal plus both spatial iterations is an
    // illegal write to an in-flight descriptor.
    if (m_tlasSrvResource == tlasResource
        && m_tlasSrvGpuVirtualAddress == tlasGpuVirtualAddress)
    {
        return true;
    }

    if (m_tlasSrvResource != nullptr)
    {
        const std::uint32_t oldIndex = m_tlasSrvIndex;
        m_tlasSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (m_tlasSrvIndex == UINT32_MAX)
        {
            m_tlasSrvIndex = oldIndex;
            outError = "failed to allocate replacement TLAS SRV descriptor";
            return false;
        }
        // The prior slot may still be referenced by an executing command list. Keep it allocated
        // until context release, which occurs after the device-shutdown GPU wait.
        m_retiredTlasSrvIndices.push_back(oldIndex);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = tlasGpuVirtualAddress;
    // RTAS SRVs take the GPUVA from the desc; pResource must be null.
    GfxContext::Get().CreateShaderResourceView(nullptr, &srvDesc, m_tlasSrvIndex);
    m_tlasSrvResource = tlasResource;
    m_tlasSrvGpuVirtualAddress = tlasGpuVirtualAddress;
    return true;
}

bool DxrDispatchContext::DispatchSmoke(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const int width,
    const int height,
    const DxrRootSignature::DispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR dispatch arguments";
        DxrLogErrorOnce("dispatch-smoke-failure", outError);
        DxrBreadcrumbOnce("dispatch-smoke-failure", std::string("dispatch failed: ") + outError);
        return false;
    }

    if (!EnsureOutput(width, height, outError))
    {
        const std::string failureMessage = std::string("EnsureOutput failed: ") + outError;
        DxrLogErrorOnce("dispatch-ensure-output-error", failureMessage);
        const std::string gpuError = GfxContext::Get().GetLastGpuAllocationError();
        if (!gpuError.empty())
        {
            DxrLogErrorOnce("dispatch-ensure-output-error", gpuError);
        }

        DxrBreadcrumbOnce(
            "dispatch-smoke-failure",
            std::string("dispatch failed: EnsureOutput (") + outError + ")");
        return false;
    }

    DxrBreadcrumb("dispatch EnsureOutput ok");
    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        DxrLogErrorOnce("dispatch-smoke-failure", outError);
        DxrBreadcrumbOnce("dispatch-smoke-failure", std::string("dispatch failed: ") + outError);
        return false;
    }

    if (m_outputUavIndex == UINT32_MAX)
    {
        outError = "DXR dispatch output UAV unavailable";
        DxrLogErrorOnce("dispatch-smoke-failure", outError);
        DxrBreadcrumbOnce("dispatch-smoke-failure", std::string("dispatch failed: ") + outError);
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR dispatch constants";
        DxrLogErrorOnce("dispatch-smoke-failure", outError);
        DxrBreadcrumbOnce("dispatch-smoke-failure", std::string("dispatch failed: ") + outError);
        return false;
    }

    DxrBreadcrumb("dispatch bind + DispatchRays begin");
    if (m_outputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_outputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_outputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_outputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);
    recorder.BindSrvTable(1, m_tlasSrvIndex);
    recorder.BindSrvTable(2, m_outputUavIndex);
    recorder.DispatchRays(shaderBindingTable, width, height);
    DxrBreadcrumb("dispatch DispatchRays submitted");
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_outputResource);

    // Combined read state: the output is consumed by pixel-shader debug blits as well as
    // future non-pixel passes; NON_PIXEL alone is invalid for the blit sample.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_outputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_outputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    DxrBreadcrumb("dispatch DispatchSmoke ok");
    return true;
}

