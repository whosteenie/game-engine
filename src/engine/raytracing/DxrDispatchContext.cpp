#include "engine/raytracing/DxrDispatchContext.h"

#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/raytracing/RestirTypes.h"
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

    RetireOrDestroyReflectionTexture(m_ptDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptDirectTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
    m_ptPrevSurfaceHistoryValid = false;

    ReleaseRetiredRestirBuffers();
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
    RetireOrDestroyStructuredBufferUav(m_restirInitialSample);
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

// DXR-03: dispatch constants are allocated per dispatch from the per-frame transient upload
// arena (256-byte aligned). A single persistent CB was previously overwritten by each dispatch,
// so with Scene View + Game View both dispatching in one frame, the GPU executed *both*
// DispatchRays with the last-written constants (wrong camera for the first view).
class DxrDispatchRecorder
{
public:
    explicit DxrDispatchRecorder(ID3D12GraphicsCommandList4* commandList) : m_commandList(commandList) {}

    void BeginDraw(
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const std::uint64_t constantsGpuAddress) const
    {
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        ID3D12DescriptorHeap* descriptorHeaps[] = {srvHeap};
        m_commandList->SetDescriptorHeaps(1, descriptorHeaps);
        m_commandList->SetPipelineState1(stateObject);
        m_commandList->SetComputeRootSignature(rootSignature);
        m_commandList->SetComputeRootConstantBufferView(0, constantsGpuAddress);
    }

    void BindSrvTable(const UINT rootParameterIndex, const std::uint32_t srvHeapIndex) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr = reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(srvHeapIndex));
        m_commandList->SetComputeRootDescriptorTable(rootParameterIndex, tableHandle);
    }

    void BindSrvTables(
        const UINT rootParameterStart,
        const std::uint32_t* srvHeapIndices,
        const UINT count) const
    {
        for (UINT tableOffset = 0; tableOffset < count; ++tableOffset)
        {
            BindSrvTable(rootParameterStart + tableOffset, srvHeapIndices[tableOffset]);
        }
    }

    void DispatchRays(const ShaderBindingTable& shaderBindingTable, const int width, const int height) const
    {
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
        m_commandList->DispatchRays(&dispatchDesc);
    }

private:
    ID3D12GraphicsCommandList4* m_commandList = nullptr;
};

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
    // PT guides retire into m_retiredReflectionOutputs (shared helper). Drain them here —
    // path-tracing mode never calls EnsureReflectionOutput, so without this every resize leaked
    // descriptors/GPU memory (G4 doubled the leak rate) until the heap exhausted and PT/DLSS
    // permanently failed.
    ReleaseRetiredReflectionOutputs();
    ReleaseRetiredRestirBuffers();
    for (RetiredOutput& retired : m_retiredPrimaryMetadata)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }
    m_retiredPrimaryMetadata.clear();

    if (m_primaryOutputResource != nullptr && m_primaryOutputWidth == width
        && m_primaryOutputHeight == height)
    {
        // Exact size match required: PT depth/motion/guides are tagged into DLSS-RR with
        // renderWidth/Height. A larger kept allocation (grow-only) makes NGX return
        // 0xbad00005 InvalidParameter ("subrect/size exceed dimensions").
        return EnsurePathTracerGuides(width, height, outError);
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
        RetireOrDestroyReflectionTexture(m_ptDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptMotionTexture);
        RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
        RetireOrDestroyReflectionTexture(m_ptDirectTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
        m_ptPrevSurfaceHistoryValid = false;
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
    if (!EnsurePathTracerGuides(width, height, outError))
    {
        return false;
    }
    return true;
}

bool DxrDispatchContext::EnsurePathTracerGuides(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid path tracer guide dimensions";
        return false;
    }

    if (m_ptDepthTexture.resource != nullptr && m_ptNormalRoughnessTexture.resource != nullptr
        && m_ptDirectTexture.resource != nullptr
        && m_ptPrevDepthTexture.resource != nullptr
        && m_ptPrevNormalRoughnessTexture.resource != nullptr
        && m_primaryOutputWidth == width && m_primaryOutputHeight == height)
    {
        return EnsureRestirBuffers(width, height, outError);
    }

    RetireOrDestroyReflectionTexture(m_ptDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptDirectTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
    m_ptPrevSurfaceHistoryValid = false;

    // Formats match the RR internal targets they are copied into (rr_guides parity —
    // devdoc/dxr/pt/full-rr-guides.md buffer contract). Prev-surface history (G4) mirrors depth +
    // normal/roughness for ReSTIR temporal validation. R2 direct is HDR bounce-0 lighting only.
    struct GuideDesc
    {
        DXGI_FORMAT format;
        ReflectionTexture* texture;
    };
    const GuideDesc guides[] = {
        {DXGI_FORMAT_R32_FLOAT, &m_ptDepthTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptMotionTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptDiffuseAlbedoTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptSpecularAlbedoTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptNormalRoughnessTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptDirectTexture},
        {DXGI_FORMAT_R32_FLOAT, &m_ptPrevDepthTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptPrevNormalRoughnessTexture},
    };
    for (const GuideDesc& guide : guides)
    {
        if (!CreateReflectionTexture(
                width,
                height,
                static_cast<std::uint32_t>(guide.format),
                *guide.texture,
                outError))
        {
            RetireOrDestroyReflectionTexture(m_ptDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptMotionTexture);
            RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
            RetireOrDestroyReflectionTexture(m_ptDirectTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
            m_ptPrevSurfaceHistoryValid = false;
            return false;
        }
    }

    return EnsureRestirBuffers(width, height, outError);
}

bool DxrDispatchContext::EnsureRestirBuffers(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid ReSTIR buffer dimensions";
        return false;
    }

    const std::uint32_t elementCount = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
    if (HasRestirBuffers() && m_restirBufferWidth == width && m_restirBufferHeight == height
        && m_restirElementCount == elementCount)
    {
        return true;
    }

    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
    RetireOrDestroyStructuredBufferUav(m_restirInitialSample);
    m_restirWriteIndex = 0;
    m_restirReservoirHistoryValid = false;
    m_restirLastSceneVersion = 0;
    m_restirBufferWidth = 0;
    m_restirBufferHeight = 0;
    m_restirElementCount = 0;

    if (!CreateStructuredBufferUav(
            elementCount, sizeof(RestirReservoir), m_restirReservoirs[0], outError)
        || !CreateStructuredBufferUav(
            elementCount, sizeof(RestirReservoir), m_restirReservoirs[1], outError)
        || !CreateStructuredBufferUav(
            elementCount, sizeof(RestirInitialSample), m_restirInitialSample, outError))
    {
        RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
        RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
        RetireOrDestroyStructuredBufferUav(m_restirInitialSample);
        return false;
    }

    m_restirBufferWidth = width;
    m_restirBufferHeight = height;
    m_restirElementCount = elementCount;
    return true;
}

bool DxrDispatchContext::CreateStructuredBufferUav(
    const std::uint32_t elementCount,
    const std::uint32_t structureByteStride,
    StructuredBufferUav& outBuffer,
    std::string& outError)
{
    outError.clear();
    outBuffer = StructuredBufferUav{};

    if (elementCount == 0 || structureByteStride == 0)
    {
        outError = "invalid structured buffer parameters";
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr || !GfxContext::Get().IsInitialized())
    {
        outError = "GfxContext unavailable for ReSTIR structured buffer";
        return false;
    }

    DxrGpuResource gpuResource{};
    const std::uint64_t byteSize =
        static_cast<std::uint64_t>(elementCount) * static_cast<std::uint64_t>(structureByteStride);
    if (!CreateDxrDefaultBuffer(
            byteSize, true, gpuResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        outError = "failed to allocate ReSTIR structured buffer ("
            + std::to_string(elementCount) + " x " + std::to_string(structureByteStride) + ")";
        return false;
    }

    outBuffer.resource = gpuResource.resource;
    outBuffer.allocation = gpuResource.allocation;
    outBuffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outBuffer.elementCount = elementCount;
    outBuffer.structureByteStride = structureByteStride;
    // Ownership transferred into outBuffer; clear so DxrGpuResource destructor does not release.
    gpuResource.resource = nullptr;
    gpuResource.allocation = nullptr;
    gpuResource.sizeInBytes = 0;

    outBuffer.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    outBuffer.uavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (outBuffer.srvIndex == UINT32_MAX || outBuffer.uavIndex == UINT32_MAX)
    {
        outError = "failed to allocate ReSTIR structured buffer descriptors";
        DestroyOutputResource(
            outBuffer.resource, outBuffer.allocation, outBuffer.srvIndex, outBuffer.uavIndex);
        outBuffer = StructuredBufferUav{};
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outBuffer.srvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = elementCount;
    srvDesc.Buffer.StructureByteStride = structureByteStride;
    device->CreateShaderResourceView(outBuffer.resource, &srvDesc, srvHandle);
    outBuffer.srvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outBuffer.uavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = elementCount;
    uavDesc.Buffer.StructureByteStride = structureByteStride;
    device->CreateUnorderedAccessView(outBuffer.resource, nullptr, &uavDesc, uavHandle);
    outBuffer.uavCpuHandle = uavHandle.ptr;
    return true;
}

void DxrDispatchContext::RetireOrDestroyStructuredBufferUav(StructuredBufferUav& buffer)
{
    if (buffer.resource == nullptr)
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        RetiredOutput retired{};
        retired.resource = buffer.resource;
        retired.allocation = buffer.allocation;
        retired.srvIndex = buffer.srvIndex;
        retired.uavIndex = buffer.uavIndex;
        m_retiredRestirBuffers.push_back(retired);
        buffer = StructuredBufferUav{};
        return;
    }

    DestroyOutputResource(buffer.resource, buffer.allocation, buffer.srvIndex, buffer.uavIndex);
    buffer = StructuredBufferUav{};
}

void DxrDispatchContext::CopyPathTracerSurfaceHistory(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr || m_ptDepthTexture.resource == nullptr
        || m_ptNormalRoughnessTexture.resource == nullptr || m_ptPrevDepthTexture.resource == nullptr
        || m_ptPrevNormalRoughnessTexture.resource == nullptr)
    {
        return;
    }

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    auto copyGuide = [&](ReflectionTexture& current, ReflectionTexture& prev) {
        TransitionResource(
            commandList,
            current.resource,
            static_cast<D3D12_RESOURCE_STATES>(current.state),
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        current.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE);

        const D3D12_RESOURCE_STATES prevBefore =
            prev.state == 0
                ? D3D12_RESOURCE_STATE_COMMON
                : static_cast<D3D12_RESOURCE_STATES>(prev.state);
        TransitionResource(
            commandList,
            prev.resource,
            prevBefore,
            D3D12_RESOURCE_STATE_COPY_DEST);
        prev.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST);

        commandList->CopyResource(prev.resource, current.resource);

        TransitionResource(
            commandList,
            current.resource,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            kAllShaderRead);
        current.state = static_cast<std::uint32_t>(kAllShaderRead);
        TransitionResource(
            commandList,
            prev.resource,
            D3D12_RESOURCE_STATE_COPY_DEST,
            kAllShaderRead);
        prev.state = static_cast<std::uint32_t>(kAllShaderRead);
    };

    copyGuide(m_ptDepthTexture, m_ptPrevDepthTexture);
    copyGuide(m_ptNormalRoughnessTexture, m_ptPrevNormalRoughnessTexture);
    m_ptPrevSurfaceHistoryValid = true;
}

void DxrDispatchContext::FinalizePathTracerSurfaceHistory(ID3D12GraphicsCommandList* commandList)
{
    CopyPathTracerSurfaceHistory(commandList);
}

void DxrDispatchContext::InvalidateRestirHistoryIfSceneChanged(const std::uint32_t sceneVersion)
{
    if (sceneVersion != m_restirLastSceneVersion)
    {
        m_restirReservoirHistoryValid = false;
        m_restirLastSceneVersion = sceneVersion;
    }
}

bool DxrDispatchContext::DispatchRestirTemporal(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const DxrRootSignature::RestirTemporalConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid ReSTIR temporal dispatch arguments";
        return false;
    }
    if (!HasRestirBuffers() || m_primaryOutputResource == nullptr || m_primaryOutputUavIndex == UINT32_MAX
        || m_ptDirectTexture.resource == nullptr || m_ptDirectTexture.srvIndex == UINT32_MAX)
    {
        outError = "ReSTIR temporal buffers unavailable";
        return false;
    }
    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const int writeIndex = m_restirWriteIndex;
    const int prevIndex = 1 - writeIndex;
    const int width = m_restirBufferWidth;
    const int height = m_restirBufferHeight;

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // g_Output must be UAV for shade rewrite; guides/motion/direct already SRV-readable after PT.
    TransitionResource(
        commandList,
        m_primaryOutputResource,
        static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_ptDirectTexture.state != static_cast<std::uint32_t>(kAllShaderRead))
    {
        TransitionResource(
            commandList,
            m_ptDirectTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDirectTexture.state),
            kAllShaderRead);
        m_ptDirectTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    auto ensureUav = [&](StructuredBufferUav& buffer) {
        if (buffer.resource == nullptr)
        {
            return;
        }
        const D3D12_RESOURCE_STATES before =
            buffer.state == 0 ? D3D12_RESOURCE_STATE_COMMON
                            : static_cast<D3D12_RESOURCE_STATES>(buffer.state);
        if (before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            TransitionResource(commandList, buffer.resource, before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            buffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    };
    ensureUav(m_restirReservoirs[writeIndex]);
    ensureUav(m_restirReservoirs[prevIndex]);
    ensureUav(m_restirInitialSample);

    DxrRootSignature::RestirTemporalConstants dispatchConstants = constants;
    const bool historyOk = m_ptPrevSurfaceHistoryValid && m_restirReservoirHistoryValid
        && dispatchConstants.historyValid != 0u;
    dispatchConstants.historyValid = historyOk ? 1u : 0u;

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(dispatchConstants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate ReSTIR temporal constants";
        return false;
    }

    DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t srvIndices[7] = {
        m_tlasSrvIndex,
        m_ptPrevDepthTexture.srvIndex,
        m_ptPrevNormalRoughnessTexture.srvIndex,
        m_ptDepthTexture.srvIndex,
        m_ptNormalRoughnessTexture.srvIndex,
        m_ptMotionTexture.srvIndex,
        m_ptDirectTexture.srvIndex};
    for (const std::uint32_t srvIndex : srvIndices)
    {
        if (srvIndex == UINT32_MAX)
        {
            outError = "ReSTIR temporal SRV bindings unavailable";
            return false;
        }
    }
    recorder.BindSrvTables(1, srvIndices, 7);

    const std::uint32_t uavIndices[4] = {
        m_restirReservoirs[writeIndex].uavIndex,
        m_restirReservoirs[prevIndex].uavIndex,
        m_restirInitialSample.uavIndex,
        m_primaryOutputUavIndex};
    for (std::uint32_t uavIndex = 0; uavIndex < 4; ++uavIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(uavIndices[uavIndex]));
        commandList->SetComputeRootDescriptorTable(8 + uavIndex, uavTableHandle);
    }

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    RecordDxrUavBarrier(commandList, m_primaryOutputResource);
    RecordDxrUavBarrier(commandList, m_restirReservoirs[writeIndex].resource);
    RecordDxrUavBarrier(commandList, m_restirInitialSample.resource);

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    m_restirWriteIndex = prevIndex;
    m_restirReservoirHistoryValid = true;
    return true;
}

bool DxrDispatchContext::DispatchRestirSpatial(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const DxrRootSignature::RestirTemporalConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid ReSTIR spatial dispatch arguments";
        return false;
    }
    if (!HasRestirBuffers() || m_primaryOutputResource == nullptr || m_primaryOutputUavIndex == UINT32_MAX
        || m_ptDirectTexture.resource == nullptr || m_ptDirectTexture.srvIndex == UINT32_MAX)
    {
        outError = "ReSTIR spatial buffers unavailable";
        return false;
    }
    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    // After temporal: writeIndex is free; temporal result is in the other slot.
    const int writeIndex = m_restirWriteIndex;
    const int readIndex = 1 - writeIndex;
    const int width = m_restirBufferWidth;
    const int height = m_restirBufferHeight;

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_ptDirectTexture.state != static_cast<std::uint32_t>(kAllShaderRead))
    {
        TransitionResource(
            commandList,
            m_ptDirectTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDirectTexture.state),
            kAllShaderRead);
        m_ptDirectTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    auto ensureUav = [&](StructuredBufferUav& buffer) {
        if (buffer.resource == nullptr)
        {
            return;
        }
        const D3D12_RESOURCE_STATES before =
            buffer.state == 0 ? D3D12_RESOURCE_STATE_COMMON
                            : static_cast<D3D12_RESOURCE_STATES>(buffer.state);
        if (before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            TransitionResource(commandList, buffer.resource, before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            buffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    };
    ensureUav(m_restirReservoirs[writeIndex]);
    ensureUav(m_restirReservoirs[readIndex]);
    ensureUav(m_restirInitialSample);

    // UAV barrier on the read buffer so this iteration sees the prior pass's writes.
    RecordDxrUavBarrier(commandList, m_restirReservoirs[readIndex].resource);

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate ReSTIR spatial constants";
        return false;
    }

    DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t srvIndices[7] = {
        m_tlasSrvIndex,
        m_ptPrevDepthTexture.srvIndex,
        m_ptPrevNormalRoughnessTexture.srvIndex,
        m_ptDepthTexture.srvIndex,
        m_ptNormalRoughnessTexture.srvIndex,
        m_ptMotionTexture.srvIndex,
        m_ptDirectTexture.srvIndex};
    for (const std::uint32_t srvIndex : srvIndices)
    {
        if (srvIndex == UINT32_MAX)
        {
            outError = "ReSTIR spatial SRV bindings unavailable";
            return false;
        }
    }
    recorder.BindSrvTables(1, srvIndices, 7);

    // u0 = spatial write, u1 = prior reservoirs (temporal / previous spatial iter).
    const std::uint32_t uavIndices[4] = {
        m_restirReservoirs[writeIndex].uavIndex,
        m_restirReservoirs[readIndex].uavIndex,
        m_restirInitialSample.uavIndex,
        m_primaryOutputUavIndex};
    for (std::uint32_t uavIndex = 0; uavIndex < 4; ++uavIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(uavIndices[uavIndex]));
        commandList->SetComputeRootDescriptorTable(8 + uavIndex, uavTableHandle);
    }

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    RecordDxrUavBarrier(commandList, m_primaryOutputResource);
    RecordDxrUavBarrier(commandList, m_restirReservoirs[writeIndex].resource);

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    // Next PT overwrites the read slot; spatial output becomes temporal history.
    m_restirWriteIndex = readIndex;
    return true;
}

bool DxrDispatchContext::CreateReflectionTexture(
    const int width,
    const int height,
    const std::uint32_t dxgiFormat,
    ReflectionTexture& outTexture,
    std::string& outError)
{
    outError.clear();

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR reflection texture";
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
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
        &outTexture.allocation,
        IID_PPV_ARGS(&outTexture.resource));
    if (FAILED(createResult))
    {
        outError = "failed to allocate DXR reflection texture ("
            + std::to_string(width) + "x" + std::to_string(height)
            + ", HRESULT=" + HresultFormat::Format(createResult) + ")";
        return false;
    }

    outTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    outTexture.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    outTexture.uavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (outTexture.srvIndex == UINT32_MAX || outTexture.uavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR reflection texture descriptors";
        DestroyOutputResource(
            outTexture.resource, outTexture.allocation, outTexture.srvIndex, outTexture.uavIndex);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outTexture.srvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(outTexture.resource, &srvDesc, srvHandle);
    outTexture.srvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outTexture.uavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    device->CreateUnorderedAccessView(outTexture.resource, nullptr, &uavDesc, uavHandle);
    return true;
}

void DxrDispatchContext::RetireOrDestroyReflectionTexture(ReflectionTexture& texture)
{
    if (texture.resource == nullptr)
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        RetiredOutput retired{};
        retired.resource = texture.resource;
        retired.allocation = texture.allocation;
        retired.srvIndex = texture.srvIndex;
        retired.uavIndex = texture.uavIndex;
        m_retiredReflectionOutputs.push_back(retired);
    }
    else
    {
        DestroyOutputResource(texture.resource, texture.allocation, texture.srvIndex, texture.uavIndex);
    }

    texture = ReflectionTexture{};
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

    const bool haveTextures = m_reflectionTextures[0].resource != nullptr;
    if (haveTextures && m_reflectionOutputWidth == width && m_reflectionOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when quality shrinks or viewports differ);
    // consumers must respect the dispatch/texture UV-scale contract (dxr-reflections.md).
    if (haveTextures && GfxContext::Get().IsFrameRecording()
        && m_reflectionOutputWidth >= width && m_reflectionOutputHeight >= height)
    {
        return true;
    }

    if (haveTextures && m_reflectionOutputWidth >= width && m_reflectionOutputHeight >= height
        && !GfxContext::Get().IsFrameRecording())
    {
        return true;
    }

    for (ReflectionTexture& texture : m_reflectionTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_reflectionOutputSrvCpuHandle = 0;
    m_reflectionDenoisedSrvCpuHandle = 0;
    m_reflectionOutputWidth = 0;
    m_reflectionOutputHeight = 0;

    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING in CMakeLists (3 = RGBA16_UNORM: 8-bit normals
    // quantize on curved surfaces and defeat RELAX's normal edge-stopping).
    const std::uint32_t formats[kReflectionTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kReflectionTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_reflectionTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_reflectionTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_reflectionOutputSrvCpuHandle = m_reflectionTextures[0].srvCpuHandle;
    m_reflectionDenoisedSrvCpuHandle = m_reflectionTextures[4].srvCpuHandle;
    m_reflectionOutputWidth = width;
    m_reflectionOutputHeight = height;
    return true;
}

DxrDispatchContext::ReflectionNrdResources DxrDispatchContext::GetReflectionNrdResources()
{
    ReflectionNrdResources resources{};
    resources.radianceHitDist = m_reflectionTextures[0].resource;
    resources.viewZ = m_reflectionTextures[1].resource;
    resources.normalRoughness = m_reflectionTextures[2].resource;
    resources.motion = m_reflectionTextures[3].resource;
    resources.denoisedOutput = m_reflectionTextures[4].resource;
    resources.radianceState = &m_reflectionTextures[0].state;
    resources.viewZState = &m_reflectionTextures[1].state;
    resources.normalRoughnessState = &m_reflectionTextures[2].state;
    resources.motionState = &m_reflectionTextures[3].state;
    resources.denoisedState = &m_reflectionTextures[4].state;
    resources.textureWidth = m_reflectionOutputWidth;
    resources.textureHeight = m_reflectionOutputHeight;
    resources.dispatchWidth = m_reflectionDispatchWidth;
    resources.dispatchHeight = m_reflectionDispatchHeight;
    return resources;
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

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
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
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
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

    // Trace writes textures [0..3] (radiance, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_reflectionTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..14 = SRV tables t0..t13 (see SerializeReflectionGlobalRootSignature).
    constexpr std::uint32_t kReflectionSrvCount = 14;
    const std::uint32_t giSrvIndex = inputs.giDenoisedSrvCpuHandle != 0
        ? DepthSrvIndexFromCpuHandle(inputs.giDenoisedSrvCpuHandle)
        : srvIndicesFromHandles[5]; // harmless fallback: RT1 indirect
    const std::uint32_t srvHeapIndices[kReflectionSrvCount] = {
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
        srvIndicesFromHandles[6],           // t10 prefiltered env cube
        srvIndicesFromHandles[7],           // t11 velocity RT4
        inputs.materialSrvIndex,            // t12 per-object material table
        giSrvIndex};                        // t13 GI denoised (optional)

    if (inputs.giDenoisedSrvCpuHandle != 0 && giSrvIndex == UINT32_MAX)
    {
        outError = "DXR reflection GI SRV binding unavailable";
        return false;
    }

    recorder.BindSrvTables(1, srvHeapIndices, kReflectionSrvCount);

    // Root params 14..17 = UAV tables u0..u3 (base = 1 + kReflectionSrvCount).
    constexpr std::uint32_t kReflectionUavCount = 4;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_reflectionTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kReflectionSrvCount + textureIndex, uavTableHandle);
    }

    // Root param 18 = bindless SRV table (space1) over the whole heap. The base must be the
    // PHYSICAL heap start (descriptor 0) so g_BindlessTextures[absoluteHeapIndex] resolves
    // correctly. GetSrvHeapGpuHandle(0) can't be used: the SRV allocator reserves the first
    // descriptors (index offset), so index 0 is "invalid" and would return null.
    {
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kReflectionSrvCount + kReflectionUavCount, bindlessHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    // Combined read state: pixel-shader debug blit + NRD compute reads (D5).
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_reflectionTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_reflectionDispatchWidth = width;
    m_reflectionDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchReflections ok");
    return true;
}

bool DxrDispatchContext::EnsureShadowOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid shadow output dimensions";
        return false;
    }

    ReleaseRetiredShadowOutputs();

    const bool haveTextures = m_shadowTextures[0].resource != nullptr;
    if (haveTextures && m_shadowOutputWidth == width && m_shadowOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when viewports differ); the shadow pass runs
    // at full render resolution so the dispatch/texture UV scale is normally 1.
    if (haveTextures && m_shadowOutputWidth >= width && m_shadowOutputHeight >= height)
    {
        return true;
    }

    for (ReflectionTexture& texture : m_shadowTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_shadowPenumbraSrvCpuHandle = 0;
    m_shadowDenoisedSrvCpuHandle = 0;
    m_shadowOutputWidth = 0;
    m_shadowOutputHeight = 0;

    // [0] penumbra, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING=3 (RGBA16_UNORM). [4] OUT_SHADOW_TRANSLUCENCY is R8+;
    // R16F keeps precision for the squared-shadow unpack and doubles as SIGMA's history buffer.
    const std::uint32_t formats[kShadowTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kShadowTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_shadowTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_shadowTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_shadowPenumbraSrvCpuHandle = m_shadowTextures[0].srvCpuHandle;
    m_shadowDenoisedSrvCpuHandle = m_shadowTextures[4].srvCpuHandle;
    m_shadowOutputWidth = width;
    m_shadowOutputHeight = height;
    return true;
}

DxrDispatchContext::ShadowNrdResources DxrDispatchContext::GetShadowNrdResources()
{
    ShadowNrdResources resources{};
    resources.penumbra = m_shadowTextures[0].resource;
    resources.viewZ = m_shadowTextures[1].resource;
    resources.normalRoughness = m_shadowTextures[2].resource;
    resources.motion = m_shadowTextures[3].resource;
    resources.denoisedOutput = m_shadowTextures[4].resource;
    resources.penumbraState = &m_shadowTextures[0].state;
    resources.viewZState = &m_shadowTextures[1].state;
    resources.normalRoughnessState = &m_shadowTextures[2].state;
    resources.motionState = &m_shadowTextures[3].state;
    resources.denoisedState = &m_shadowTextures[4].state;
    resources.textureWidth = m_shadowOutputWidth;
    resources.textureHeight = m_shadowOutputHeight;
    resources.dispatchWidth = m_shadowDispatchWidth;
    resources.dispatchHeight = m_shadowDispatchHeight;
    return resources;
}

bool DxrDispatchContext::DispatchShadows(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ShadowDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ShadowDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR shadow dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[4] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR shadow SRV bindings unavailable";
            return false;
        }
    }

    if (!EnsureShadowOutput(width, height, outError))
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
        outError = "failed to allocate transient DXR shadow constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // Raygen writes textures [0..3] (penumbra, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_shadowTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..5 = SRV tables t0..t4 (see SerializeShadowGlobalRootSignature).
    constexpr std::uint32_t kShadowSrvCount = 5;
    const std::uint32_t srvHeapIndices[kShadowSrvCount] = {
        m_tlasSrvIndex,           // t0 TLAS
        srvIndicesFromHandles[0], // t1 depth
        srvIndicesFromHandles[1], // t2 shading normal
        srvIndicesFromHandles[2], // t3 material0 (roughness)
        srvIndicesFromHandles[3]};// t4 velocity

    recorder.BindSrvTables(1, srvHeapIndices, kShadowSrvCount);

    // Root params 6..9 = UAV tables u0..u3 (base = 1 + kShadowSrvCount).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_shadowTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kShadowSrvCount + textureIndex, uavTableHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    // Combined read state: pixel-shader debug blit + NRD compute reads.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_shadowTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_shadowDispatchWidth = width;
    m_shadowDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchShadows ok");
    return true;
}

bool DxrDispatchContext::EnsureGiOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid GI output dimensions";
        return false;
    }

    ReleaseRetiredGiOutputs();

    const bool haveTextures = m_giTextures[0].resource != nullptr;
    if (haveTextures && m_giOutputWidth == width && m_giOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when quality shrinks / viewports differ);
    // consumers respect the dispatch/texture UV-scale contract.
    if (haveTextures && m_giOutputWidth >= width && m_giOutputHeight >= height)
    {
        return true;
    }

    for (ReflectionTexture& texture : m_giTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_giOutputSrvCpuHandle = 0;
    m_giDenoisedSrvCpuHandle = 0;
    m_giOutputWidth = 0;
    m_giOutputHeight = 0;

    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING=3 (RGBA16_UNORM), same as the reflection set.
    const std::uint32_t formats[kGiTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kGiTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_giTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_giTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_giOutputSrvCpuHandle = m_giTextures[0].srvCpuHandle;
    m_giDenoisedSrvCpuHandle = m_giTextures[4].srvCpuHandle;
    m_giOutputWidth = width;
    m_giOutputHeight = height;
    return true;
}

DxrDispatchContext::ReflectionNrdResources DxrDispatchContext::GetGiNrdResources()
{
    ReflectionNrdResources resources{};
    resources.radianceHitDist = m_giTextures[0].resource;
    resources.viewZ = m_giTextures[1].resource;
    resources.normalRoughness = m_giTextures[2].resource;
    resources.motion = m_giTextures[3].resource;
    resources.denoisedOutput = m_giTextures[4].resource;
    resources.radianceState = &m_giTextures[0].state;
    resources.viewZState = &m_giTextures[1].state;
    resources.normalRoughnessState = &m_giTextures[2].state;
    resources.motionState = &m_giTextures[3].state;
    resources.denoisedState = &m_giTextures[4].state;
    resources.textureWidth = m_giOutputWidth;
    resources.textureHeight = m_giOutputHeight;
    resources.dispatchWidth = m_giDispatchWidth;
    resources.dispatchHeight = m_giDispatchHeight;
    return resources;
}

bool DxrDispatchContext::DispatchGi(
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
        outError = "invalid DXR GI dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR GI SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
    {
        outError = "DXR GI geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsureGiOutput(width, height, outError))
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
        outError = "failed to allocate transient DXR GI constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // Trace writes textures [0..3] (radiance, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_giTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..14 = SRV tables t0..t13 (reflection global root signature layout).
    constexpr std::uint32_t kGiSrvCount = 14;
    const std::uint32_t srvHeapIndices[kGiSrvCount] = {
        m_tlasSrvIndex,                     // t0 TLAS
        srvIndicesFromHandles[0],           // t1 depth
        srvIndicesFromHandles[1],           // t2 shading normal
        srvIndicesFromHandles[2],           // t3 material0
        inputs.geometryLookupSrvIndex,      // t4
        inputs.sceneVertexFloatsSrvIndex,   // t5
        inputs.sceneIndicesSrvIndex,        // t6
        srvIndicesFromHandles[3],           // t7 direct RT0 (unused by GI shader; bound for parity)
        srvIndicesFromHandles[4],           // t8 sun shadow RT3 (unused)
        srvIndicesFromHandles[5],           // t9 indirect RT1 (unused)
        srvIndicesFromHandles[6],           // t10 prefiltered env cube
        srvIndicesFromHandles[7],           // t11 velocity RT4
        inputs.materialSrvIndex,            // t12 per-object material table
        srvIndicesFromHandles[5]};          // t13 GI unused by GI shader; bind RT1 for parity

    recorder.BindSrvTables(1, srvHeapIndices, kGiSrvCount);

    // Root params 14..17 = UAV tables u0..u3 (GI texture set).
    constexpr std::uint32_t kGiUavCount = 4;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_giTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kGiSrvCount + textureIndex, uavTableHandle);
    }

    // Root param 18 = bindless SRV table (space1) over the whole heap (physical heap start).
    {
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kGiSrvCount + kGiUavCount, bindlessHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_giTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_giDispatchWidth = width;
    m_giDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchGi ok");
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

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t srvIndices[5] = {
        m_tlasSrvIndex,
        depthSrvIndex,
        geometryLookupSrvIndex,
        sceneVertexFloatsSrvIndex,
        sceneIndicesSrvIndex};
    recorder.BindSrvTables(1, srvIndices, 5);
    recorder.BindSrvTable(6, m_primaryOutputUavIndex);
    recorder.BindSrvTable(7, m_primaryMetadataUavIndex);

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);
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

bool DxrDispatchContext::DispatchPathTracer(
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
        outError = "invalid DXR path tracer dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR path tracer SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
    {
        outError = "DXR path tracer geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsurePrimaryOutput(width, height, outError))
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
        outError = "failed to allocate transient DXR path tracer constants";
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

    if (m_ptDepthTexture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_ptDepthTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDepthTexture.state),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_ptDepthTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_ptMotionTexture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_ptMotionTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptMotionTexture.state),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_ptMotionTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ReflectionTexture* const ptGuideTextures[] = {
        &m_ptDiffuseAlbedoTexture,
        &m_ptSpecularAlbedoTexture,
        &m_ptNormalRoughnessTexture,
        &m_ptDirectTexture};
    for (ReflectionTexture* guide : ptGuideTextures)
    {
        if (guide->state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                guide->resource,
                static_cast<D3D12_RESOURCE_STATES>(guide->state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            guide->state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Path-tracer root signature: t0-t18 (reflection set + prev transforms + emissive NEE + env IS).
    constexpr std::uint32_t kPathTracerSrvCount = 19;
    const std::uint32_t giSrvIndex = inputs.giDenoisedSrvCpuHandle != 0
        ? DepthSrvIndexFromCpuHandle(inputs.giDenoisedSrvCpuHandle)
        : srvIndicesFromHandles[5];
    // t14 needs a valid descriptor even when instance motion is unavailable (the shader is told
    // via constants and never reads it then) — geometry lookup is a safe placeholder buffer SRV.
    const std::uint32_t prevTransformsSrvIndex =
        inputs.prevInstanceTransformsSrvIndex != UINT32_MAX
            ? inputs.prevInstanceTransformsSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveLightsSrvIndex =
        inputs.emissiveLightsSrvIndex != UINT32_MAX
            ? inputs.emissiveLightsSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveTrianglesSrvIndex =
        inputs.emissiveTrianglesSrvIndex != UINT32_MAX
            ? inputs.emissiveTrianglesSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t envImportanceCdfSrvIndex =
        inputs.envImportanceCdfSrvIndex != UINT32_MAX
            ? inputs.envImportanceCdfSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t envEquirectSrvIndex =
        inputs.envEquirectSrvCpuHandle != 0
            ? DepthSrvIndexFromCpuHandle(inputs.envEquirectSrvCpuHandle)
            : srvIndicesFromHandles[6];
    const std::uint32_t srvHeapIndices[kPathTracerSrvCount] = {
        m_tlasSrvIndex,
        srvIndicesFromHandles[0],
        srvIndicesFromHandles[1],
        srvIndicesFromHandles[2],
        inputs.geometryLookupSrvIndex,
        inputs.sceneVertexFloatsSrvIndex,
        inputs.sceneIndicesSrvIndex,
        srvIndicesFromHandles[3],
        srvIndicesFromHandles[4],
        srvIndicesFromHandles[5],
        srvIndicesFromHandles[6],
        srvIndicesFromHandles[7],
        inputs.materialSrvIndex,
        giSrvIndex,
        prevTransformsSrvIndex,
        emissiveLightsSrvIndex,
        envImportanceCdfSrvIndex,
        envEquirectSrvIndex,
        emissiveTrianglesSrvIndex};

    if (inputs.giDenoisedSrvCpuHandle != 0 && giSrvIndex == UINT32_MAX)
    {
        outError = "DXR path tracer GI SRV binding unavailable";
        return false;
    }

    recorder.BindSrvTables(1, srvHeapIndices, kPathTracerSrvCount);

    if (!HasRestirBuffers()
        || m_restirInitialSample.uavIndex == UINT32_MAX
        || m_restirReservoirs[m_restirWriteIndex].uavIndex == UINT32_MAX
        || m_ptDirectTexture.uavIndex == UINT32_MAX)
    {
        outError = "DXR path tracer ReSTIR buffer UAVs unavailable";
        return false;
    }

    // Path-tracer root signature: u0-u6 RR guides, u7 InitialSample, u8 Reservoir[write], u9 direct.
    constexpr std::uint32_t kPathTracerUavCount = 10;
    const std::uint32_t pathTracerUavIndices[kPathTracerUavCount] = {
        m_primaryOutputUavIndex,
        m_ptDepthTexture.uavIndex,
        m_primaryMetadataUavIndex,
        m_ptMotionTexture.uavIndex,
        m_ptDiffuseAlbedoTexture.uavIndex,
        m_ptSpecularAlbedoTexture.uavIndex,
        m_ptNormalRoughnessTexture.uavIndex,
        m_restirInitialSample.uavIndex,
        m_restirReservoirs[m_restirWriteIndex].uavIndex,
        m_ptDirectTexture.uavIndex};
    for (std::uint32_t uavIndex = 0; uavIndex < kPathTracerUavCount; ++uavIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(pathTracerUavIndices[uavIndex]));
        commandList->SetComputeRootDescriptorTable(1 + kPathTracerSrvCount + uavIndex, uavTableHandle);
    }

    {
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kPathTracerSrvCount + kPathTracerUavCount, bindlessHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryOutputResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryMetadataResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_ptDepthTexture.resource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_ptMotionTexture.resource);
    RecordDxrUavBarrier(
        static_cast<ID3D12GraphicsCommandList*>(commandList), m_restirInitialSample.resource);
    RecordDxrUavBarrier(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_restirReservoirs[m_restirWriteIndex].resource);

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_ptDepthTexture.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_ptDepthTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_ptMotionTexture.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_ptMotionTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    for (ReflectionTexture* guide : ptGuideTextures)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), guide->resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            guide->resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        guide->state = static_cast<std::uint32_t>(kAllShaderRead);
    }
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

    // G4 copy + R2 temporal are owned by DxrPathTracerDispatch after this returns so temporal
    // can run before prev-surface history is overwritten.
    DxrBreadcrumb("dispatch DispatchPathTracer ok");
    return true;
}
