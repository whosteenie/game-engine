#include "engine/raytracing/DxrDispatchContext.h"

#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/raytracing/ShaderBindingTable.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/HresultFormat.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstring>

namespace
{
    void DestroyOutputResource(
        ID3D12Resource*& resource,
        D3D12MA::Allocation*& allocation,
        std::uint32_t& srvIndex,
        std::uint32_t& uavIndex)
    {
        // CRASH-01/CRASH-03: defer release + descriptor recycling until the covering fence
        // completes; an in-flight or currently recording command list may still reference them.
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
            srvIndex = UINT32_MAX;
        }

        if (uavIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(uavIndex);
            uavIndex = UINT32_MAX;
        }

        if (allocation != nullptr || resource != nullptr)
        {
            GfxContext::Get().DeferredReleaseResource(allocation, resource);
            allocation = nullptr;
        }

        resource = nullptr;
    }
}

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

    ReleaseRetiredReflectionOutputs();
    DestroyOutputResource(
        m_reflectionOutputResource,
        m_reflectionOutputAllocation,
        m_reflectionOutputSrvIndex,
        m_reflectionOutputUavIndex);
    m_reflectionOutputSrvCpuHandle = 0;
    m_reflectionOutputWidth = 0;
    m_reflectionOutputHeight = 0;
    m_reflectionOutputResourceState = 0;

    if (m_tlasSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(m_tlasSrvIndex);
        m_tlasSrvIndex = UINT32_MAX;
    }

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

// DXR-03: dispatch constants are allocated per dispatch from the per-frame transient upload
// arena (256-byte aligned). A single persistent CB was previously overwritten by each dispatch,
// so with Scene View + Game View both dispatching in one frame, the GPU executed *both*
// DispatchRays with the last-written constants (wrong camera for the first view).
namespace
{
    template <typename TConstants>
    std::uint64_t AllocateDispatchConstants(const TConstants& constants)
    {
        const GfxContext::TransientUploadAllocation allocation =
            GfxContext::Get().AllocateTransientUpload(&constants, sizeof(TConstants));
        return allocation.gpuAddress;
    }
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
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    if (device == nullptr || srvHeap == nullptr || m_outputResource == nullptr)
    {
        return;
    }

    const std::uint32_t descriptorSize = GfxContext::Get().GetSrvDescriptorSize();

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_outputSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_outputResource, &srvDesc, srvHandle);
    m_outputSrvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_outputUavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(m_outputResource, nullptr, &uavDesc, uavHandle);
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

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        outError = "D3D12 device unavailable for TLAS SRV creation";
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_tlasSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = tlasGpuVirtualAddress;
    // RTAS SRVs take the GPUVA from the desc; pResource must be null.
    device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);
    (void)tlasResource;
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
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    if (m_outputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_outputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_outputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_outputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    ID3D12DescriptorHeap* descriptorHeaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->SetPipelineState1(stateObject);
    commandList->SetComputeRootSignature(rootSignature);
    commandList->SetComputeRootConstantBufferView(0, constantsGpuAddress);

    D3D12_GPU_DESCRIPTOR_HANDLE srvTableHandle{};
    srvTableHandle.ptr = reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(m_tlasSrvIndex));
    commandList->SetComputeRootDescriptorTable(1, srvTableHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
    uavTableHandle.ptr = reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(m_outputUavIndex));
    commandList->SetComputeRootDescriptorTable(2, uavTableHandle);

    D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = shaderBindingTable.GetRaygenGpuAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatchDesc.MissShaderTable.StartAddress = shaderBindingTable.GetMissGpuAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.MissShaderTable.StrideInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.HitGroupTable.StartAddress = shaderBindingTable.GetHitGroupGpuAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.HitGroupTable.StrideInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.Width = static_cast<UINT>(width);
    dispatchDesc.Height = static_cast<UINT>(height);
    dispatchDesc.Depth = 1;

    commandList->DispatchRays(&dispatchDesc);
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

void DxrDispatchContext::ReleaseRetiredPrimaryOutputs()
{
    for (RetiredOutput& retired : m_retiredPrimaryOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredPrimaryOutputs.clear();
}

void DxrDispatchContext::CreatePrimaryOutputDescriptors()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr || m_primaryOutputResource == nullptr || m_primaryMetadataResource == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE outputSrvHandle{};
    outputSrvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryOutputSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC outputSrvDesc{};
    outputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outputSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outputSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_primaryOutputResource, &outputSrvDesc, outputSrvHandle);
    m_primaryOutputSrvCpuHandle = outputSrvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE outputUavHandle{};
    outputUavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryOutputUavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc{};
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    outputUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(m_primaryOutputResource, nullptr, &outputUavDesc, outputUavHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE metadataSrvHandle{};
    metadataSrvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryMetadataSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC metadataSrvDesc{};
    metadataSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    metadataSrvDesc.Format = DXGI_FORMAT_R32G32_UINT;
    metadataSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    metadataSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_primaryMetadataResource, &metadataSrvDesc, metadataSrvHandle);
    m_primaryMetadataSrvCpuHandle = metadataSrvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE metadataUavHandle{};
    metadataUavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryMetadataUavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC metadataUavDesc{};
    metadataUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    metadataUavDesc.Format = DXGI_FORMAT_R32G32_UINT;
    device->CreateUnorderedAccessView(m_primaryMetadataResource, nullptr, &metadataUavDesc, metadataUavHandle);
}

bool DxrDispatchContext::EnsurePrimaryOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid primary debug output dimensions";
        return false;
    }

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

    if (m_primaryOutputResource != nullptr && m_primaryOutputWidth == width && m_primaryOutputHeight == height)
    {
        return true;
    }

    if (m_primaryOutputResource != nullptr)
    {
        RetiredOutput retiredOutput{};
        retiredOutput.resource = m_primaryOutputResource;
        retiredOutput.allocation = m_primaryOutputAllocation;
        retiredOutput.srvIndex = m_primaryOutputSrvIndex;
        retiredOutput.uavIndex = m_primaryOutputUavIndex;
        m_retiredPrimaryOutputs.push_back(retiredOutput);

        RetiredOutput retiredMetadata{};
        retiredMetadata.resource = m_primaryMetadataResource;
        retiredMetadata.allocation = m_primaryMetadataAllocation;
        retiredMetadata.srvIndex = m_primaryMetadataSrvIndex;
        retiredMetadata.uavIndex = m_primaryMetadataUavIndex;
        m_retiredPrimaryMetadata.push_back(retiredMetadata);

        m_primaryOutputResource = nullptr;
        m_primaryOutputAllocation = nullptr;
        m_primaryMetadataResource = nullptr;
        m_primaryMetadataAllocation = nullptr;
        m_primaryOutputSrvIndex = UINT32_MAX;
        m_primaryOutputUavIndex = UINT32_MAX;
        m_primaryMetadataSrvIndex = UINT32_MAX;
        m_primaryMetadataUavIndex = UINT32_MAX;
        m_primaryOutputSrvCpuHandle = 0;
        m_primaryMetadataSrvCpuHandle = 0;
        m_primaryOutputWidth = 0;
        m_primaryOutputHeight = 0;
        m_primaryOutputResourceState = 0;
        m_primaryMetadataResourceState = 0;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR primary debug output textures";
        return false;
    }

    D3D12_RESOURCE_DESC outputDesc{};
    outputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    outputDesc.Width = static_cast<UINT64>(width);
    outputDesc.Height = static_cast<UINT>(height);
    outputDesc.DepthOrArraySize = 1;
    outputDesc.MipLevels = 1;
    outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &outputDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &m_primaryOutputAllocation,
            IID_PPV_ARGS(&m_primaryOutputResource))))
    {
        outError = "failed to allocate DXR primary debug output texture";
        return false;
    }

    D3D12_RESOURCE_DESC metadataDesc = outputDesc;
    metadataDesc.Format = DXGI_FORMAT_R32G32_UINT;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &metadataDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &m_primaryMetadataAllocation,
            IID_PPV_ARGS(&m_primaryMetadataResource))))
    {
        outError = "failed to allocate DXR primary debug metadata texture";
        DestroyOutputResource(
            m_primaryOutputResource,
            m_primaryOutputAllocation,
            m_primaryOutputSrvIndex,
            m_primaryOutputUavIndex);
        return false;
    }

    m_primaryOutputWidth = width;
    m_primaryOutputHeight = height;
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);
    m_primaryMetadataResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    m_primaryOutputSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryOutputUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryMetadataSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryMetadataUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (m_primaryOutputSrvIndex == UINT32_MAX || m_primaryOutputUavIndex == UINT32_MAX
        || m_primaryMetadataSrvIndex == UINT32_MAX || m_primaryMetadataUavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR primary debug output descriptors";
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
        return false;
    }

    CreatePrimaryOutputDescriptors();
    return true;
}

bool DxrDispatchContext::EnsureReflectionOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid reflection output dimensions";
        return false;
    }

    ReleaseRetiredReflectionOutputs();

    if (m_reflectionOutputResource != nullptr
        && m_reflectionOutputWidth == width && m_reflectionOutputHeight == height)
    {
        return true;
    }

    if (m_reflectionOutputResource != nullptr && GfxContext::Get().IsFrameRecording())
    {
        // Same command list may already reference the current output (scene view then game view).
        if (m_reflectionOutputWidth >= width && m_reflectionOutputHeight >= height)
        {
            return true;
        }

        RetiredOutput retired{};
        retired.resource = m_reflectionOutputResource;
        retired.allocation = m_reflectionOutputAllocation;
        retired.srvIndex = m_reflectionOutputSrvIndex;
        retired.uavIndex = m_reflectionOutputUavIndex;
        m_retiredReflectionOutputs.push_back(retired);

        m_reflectionOutputResource = nullptr;
        m_reflectionOutputAllocation = nullptr;
        m_reflectionOutputSrvIndex = UINT32_MAX;
        m_reflectionOutputUavIndex = UINT32_MAX;
        m_reflectionOutputSrvCpuHandle = 0;
        m_reflectionOutputWidth = 0;
        m_reflectionOutputHeight = 0;
        m_reflectionOutputResourceState = 0;
    }
    else if (m_reflectionOutputResource != nullptr)
    {
        DestroyOutputResource(
            m_reflectionOutputResource,
            m_reflectionOutputAllocation,
            m_reflectionOutputSrvIndex,
            m_reflectionOutputUavIndex);
        m_reflectionOutputSrvCpuHandle = 0;
        m_reflectionOutputWidth = 0;
        m_reflectionOutputHeight = 0;
        m_reflectionOutputResourceState = 0;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR reflection output texture";
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
        &m_reflectionOutputAllocation,
        IID_PPV_ARGS(&m_reflectionOutputResource));
    if (FAILED(createResult))
    {
        outError = "failed to allocate DXR reflection output texture ("
            + std::to_string(width) + "x" + std::to_string(height)
            + ", HRESULT=" + HresultFormat::Format(createResult) + ")";
        return false;
    }

    m_reflectionOutputWidth = width;
    m_reflectionOutputHeight = height;
    m_reflectionOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    m_reflectionOutputSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_reflectionOutputUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (m_reflectionOutputSrvIndex == UINT32_MAX || m_reflectionOutputUavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR reflection output descriptors";
        DestroyOutputResource(
            m_reflectionOutputResource,
            m_reflectionOutputAllocation,
            m_reflectionOutputSrvIndex,
            m_reflectionOutputUavIndex);
        return false;
    }

    CreateReflectionOutputDescriptors();
    return true;
}

void DxrDispatchContext::CreateReflectionOutputDescriptors()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr || m_reflectionOutputResource == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_reflectionOutputSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_reflectionOutputResource, &srvDesc, srvHandle);
    m_reflectionOutputSrvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_reflectionOutputUavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(m_reflectionOutputResource, nullptr, &uavDesc, uavHandle);
}

bool DxrDispatchContext::DispatchReflections(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ReflectionDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ReflectionDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR reflection dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[7] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR reflection SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX)
    {
        outError = "DXR reflection geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsureReflectionOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(inputs.tlasResource, inputs.tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR reflection constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    if (m_reflectionOutputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_reflectionOutputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_reflectionOutputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_reflectionOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    ID3D12DescriptorHeap* descriptorHeaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->SetPipelineState1(stateObject);
    commandList->SetComputeRootSignature(rootSignature);
    commandList->SetComputeRootConstantBufferView(0, constantsGpuAddress);

    // Root params 1..11 = SRV tables t0..t10 (see SerializeReflectionGlobalRootSignature).
    const std::uint32_t srvHeapIndices[11] = {
        m_tlasSrvIndex,                     // t0 TLAS
        srvIndicesFromHandles[0],           // t1 depth
        srvIndicesFromHandles[1],           // t2 shading normal
        srvIndicesFromHandles[2],           // t3 material0
        inputs.geometryLookupSrvIndex,      // t4
        inputs.sceneVertexFloatsSrvIndex,   // t5
        inputs.sceneIndicesSrvIndex,        // t6
        srvIndicesFromHandles[3],           // t7 direct RT0
        srvIndicesFromHandles[4],           // t8 sun shadow RT3
        srvIndicesFromHandles[5],           // t9 indirect RT1
        srvIndicesFromHandles[6]};          // t10 prefiltered env cube

    for (std::uint32_t rootIndex = 0; rootIndex < 11; ++rootIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr =
            reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(srvHeapIndices[rootIndex]));
        commandList->SetComputeRootDescriptorTable(1 + rootIndex, tableHandle);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
    uavTableHandle.ptr =
        reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(m_reflectionOutputUavIndex));
    commandList->SetComputeRootDescriptorTable(12, uavTableHandle);

    D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = shaderBindingTable.GetRaygenGpuAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatchDesc.MissShaderTable.StartAddress = shaderBindingTable.GetMissGpuAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.MissShaderTable.StrideInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.HitGroupTable.StartAddress = shaderBindingTable.GetHitGroupGpuAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.HitGroupTable.StrideInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.Width = static_cast<UINT>(width);
    dispatchDesc.Height = static_cast<UINT>(height);
    dispatchDesc.Depth = 1;

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    commandList->DispatchRays(&dispatchDesc);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_reflectionOutputResource);

    // Combined read state: pixel-shader debug blit + future D5/D6 consumers.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_reflectionOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_reflectionOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    m_reflectionDispatchWidth = width;
    m_reflectionDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchReflections ok");
    return true;
}

std::uint32_t DxrDispatchContext::DepthSrvIndexFromCpuHandle(
    const std::uintptr_t depthSrvCpuHandle) const
{
    if (depthSrvCpuHandle == 0)
    {
        return UINT32_MAX;
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    if (srvHeap == nullptr)
    {
        return UINT32_MAX;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetCPUDescriptorHandleForHeapStart();
    const std::uint32_t descriptorSize = GfxContext::Get().GetSrvDescriptorSize();
    return static_cast<std::uint32_t>((depthSrvCpuHandle - heapStart.ptr) / descriptorSize);
}

bool DxrDispatchContext::DispatchPrimaryDebug(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const std::uintptr_t depthSrvCpuHandle,
    const std::uint32_t geometryLookupSrvIndex,
    const std::uint32_t sceneVertexFloatsSrvIndex,
    const std::uint32_t sceneIndicesSrvIndex,
    const int width,
    const int height,
    const DxrRootSignature::PrimaryDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR primary debug dispatch arguments";
        return false;
    }

    const std::uint32_t depthSrvIndex = DepthSrvIndexFromCpuHandle(depthSrvCpuHandle);
    if (depthSrvIndex == UINT32_MAX || geometryLookupSrvIndex == UINT32_MAX
        || sceneVertexFloatsSrvIndex == UINT32_MAX || sceneIndicesSrvIndex == UINT32_MAX)
    {
        outError = "DXR primary debug SRV bindings unavailable";
        return false;
    }

    if (!EnsurePrimaryOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR primary debug constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    if (m_primaryOutputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryOutputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_primaryMetadataResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryMetadataResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryMetadataResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryMetadataResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    ID3D12DescriptorHeap* descriptorHeaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    commandList->SetPipelineState1(stateObject);
    commandList->SetComputeRootSignature(rootSignature);
    commandList->SetComputeRootConstantBufferView(0, constantsGpuAddress);

    const std::uint32_t srvIndices[5] = {
        m_tlasSrvIndex,
        depthSrvIndex,
        geometryLookupSrvIndex,
        sceneVertexFloatsSrvIndex,
        sceneIndicesSrvIndex};

    for (std::uint32_t rootIndex = 0; rootIndex < 5; ++rootIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr =
            reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(srvIndices[rootIndex]));
        commandList->SetComputeRootDescriptorTable(1 + rootIndex, tableHandle);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE outputUavHandle{};
    outputUavHandle.ptr =
        reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(m_primaryOutputUavIndex));
    commandList->SetComputeRootDescriptorTable(6, outputUavHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE metadataUavHandle{};
    metadataUavHandle.ptr =
        reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(m_primaryMetadataUavIndex));
    commandList->SetComputeRootDescriptorTable(7, metadataUavHandle);

    D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = shaderBindingTable.GetRaygenGpuAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatchDesc.MissShaderTable.StartAddress = shaderBindingTable.GetMissGpuAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.MissShaderTable.StrideInBytes = shaderBindingTable.GetMissRecordStride();
    dispatchDesc.HitGroupTable.StartAddress = shaderBindingTable.GetHitGroupGpuAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.HitGroupTable.StrideInBytes = shaderBindingTable.GetHitGroupRecordStride();
    dispatchDesc.Width = static_cast<UINT>(width);
    dispatchDesc.Height = static_cast<UINT>(height);
    dispatchDesc.Depth = 1;

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    commandList->DispatchRays(&dispatchDesc);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryOutputResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryMetadataResource);

    // Combined read state — these buffers are sampled by the pixel-shader debug blit.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryMetadataResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryMetadataResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    DxrBreadcrumb("dispatch DispatchPrimaryDebug ok");
    return true;
}
