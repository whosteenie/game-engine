#include "engine/raytracing/DxrDispatchContext.h"

#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/raytracing/ShaderBindingTable.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstring>
#include <sstream>
#include <iomanip>

namespace
{
    std::string FormatHresult(const HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
        return stream.str();
    }
    std::uint64_t AlignConstantBufferSize(const std::uint64_t byteSize)
    {
        return (byteSize + 255ull) & ~255ull;
    }

    void DestroyOutputResource(
        ID3D12Resource*& resource,
        D3D12MA::Allocation*& allocation,
        std::uint32_t& srvIndex,
        std::uint32_t& uavIndex)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().FreeOffscreenSrv(srvIndex);
            srvIndex = UINT32_MAX;
        }

        if (uavIndex != UINT32_MAX)
        {
            GfxContext::Get().FreeOffscreenSrv(uavIndex);
            uavIndex = UINT32_MAX;
        }

        if (allocation != nullptr)
        {
            allocation->Release();
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

    if (m_tlasSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(m_tlasSrvIndex);
        m_tlasSrvIndex = UINT32_MAX;
    }

    if (m_constantBufferAllocation != nullptr)
    {
        m_constantBufferAllocation->Release();
        m_constantBufferAllocation = nullptr;
    }

    m_constantBufferResource = nullptr;
}

bool DxrDispatchContext::EnsureConstantBuffer(std::string& outError)
{
    outError.clear();
    if (m_constantBufferResource != nullptr)
    {
        return true;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        outError = "memory allocator unavailable";
        return false;
    }

    const std::uint64_t bufferSize = AlignConstantBufferSize(sizeof(DxrRootSignature::DispatchConstants));

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = bufferSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &m_constantBufferAllocation,
            IID_PPV_ARGS(&m_constantBufferResource))))
    {
        outError = "failed to allocate DXR dispatch constant buffer";
        return false;
    }

    return true;
}

void DxrDispatchContext::WriteConstantBuffer(const DxrRootSignature::DispatchConstants& constants)
{
    if (m_constantBufferResource == nullptr)
    {
        return;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(m_constantBufferResource->Map(0, nullptr, &mapped)))
    {
        std::memcpy(mapped, &constants, sizeof(constants));
        m_constantBufferResource->Unmap(0, nullptr);
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
        return EnsureConstantBuffer(outError);
    }

    if (m_outputResource != nullptr && GfxContext::Get().IsFrameRecording())
    {
        // Same command list may already reference the current output (e.g. scene view then game view).
        if (m_outputWidth >= width && m_outputHeight >= height)
        {
            return EnsureConstantBuffer(outError);
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
            + ", HRESULT=" + FormatHresult(createResult) + ")";
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
    return EnsureConstantBuffer(outError);
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

    if (m_outputUavIndex == UINT32_MAX || m_constantBufferResource == nullptr)
    {
        outError = "DXR dispatch output UAV or constant buffer unavailable";
        DxrLogErrorOnce("dispatch-smoke-failure", outError);
        DxrBreadcrumbOnce("dispatch-smoke-failure", std::string("dispatch failed: ") + outError);
        return false;
    }

    WriteConstantBuffer(constants);

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
    commandList->SetComputeRootConstantBufferView(
        0,
        m_constantBufferResource->GetGPUVirtualAddress());

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

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_outputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_outputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    DxrBreadcrumb("dispatch DispatchSmoke ok");
    return true;
}
