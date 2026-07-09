#include "engine/raytracing/DxrGpuResource.h"

#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include "engine/raytracing/DxrHeaders.h"

namespace
{
    std::uint64_t AlignBufferSize(const std::uint64_t byteSize)
    {
        return (byteSize + 255ull) & ~255ull;
    }
}

void DxrGpuResource::Release()
{
    // CRASH-01: BLAS/TLAS results, scratch, and upload buffers are released on growth mid-frame
    // while a recording or in-flight command list may still reference them; defer to the fence.
    if (allocation != nullptr || resource != nullptr)
    {
        GfxContext::Get().DeferredReleaseResource(allocation, resource);
    }

    allocation = nullptr;
    resource = nullptr;
    sizeInBytes = 0;
}

std::uint64_t DxrGpuResource::GetGpuVirtualAddress() const
{
    return resource != nullptr ? resource->GetGPUVirtualAddress() : 0ull;
}

bool CreateDxrDefaultBuffer(
    const std::uint64_t sizeInBytes,
    const bool allowUnorderedAccess,
    DxrGpuResource& outResource,
    const D3D12_RESOURCE_STATES initialState)
{
    outResource.Release();
    if (sizeInBytes == 0 || !GfxContext::Get().IsInitialized())
    {
        return false;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        return false;
    }

    const std::uint64_t alignedSize = AlignBufferSize(sizeInBytes);

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = alignedSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags =
        allowUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    const HRESULT createResult = allocator->CreateResource(
        &allocationDesc,
        &resourceDesc,
        initialState,
        nullptr,
        &allocation,
        IID_PPV_ARGS(&resource));
    if (FAILED(createResult))
    {
        return false;
    }

    outResource.resource = resource;
    outResource.allocation = allocation;
    outResource.sizeInBytes = alignedSize;
    return true;
}

bool CreateDxrScratchBuffer(const std::uint64_t sizeInBytes, DxrGpuResource& outResource)
{
    return CreateDxrDefaultBuffer(
        sizeInBytes,
        true,
        outResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool CreateDxrUploadBuffer(const std::uint64_t sizeInBytes, DxrGpuResource& outResource)
{
    outResource.Release();
    if (sizeInBytes == 0 || !GfxContext::Get().IsInitialized())
    {
        return false;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        return false;
    }

    const std::uint64_t alignedSize = AlignBufferSize(sizeInBytes);

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = alignedSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    const HRESULT createResult = allocator->CreateResource(
        &allocationDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        &allocation,
        IID_PPV_ARGS(&resource));
    if (FAILED(createResult))
    {
        return false;
    }

    outResource.resource = resource;
    outResource.allocation = allocation;
    outResource.sizeInBytes = alignedSize;
    return true;
}

void DxrUploadRing::Release()
{
    for (DxrGpuResource& slot : m_slots)
    {
        slot.Release();
    }

    m_capacity = 0;
}

DxrGpuResource& DxrUploadRing::Slot(const std::uint32_t frameIndex)
{
    return m_slots[frameIndex % GfxContext::FrameCount];
}

const DxrGpuResource& DxrUploadRing::Slot(const std::uint32_t frameIndex) const
{
    return m_slots[frameIndex % GfxContext::FrameCount];
}

bool DxrUploadRing::EnsureCapacity(const std::uint64_t sizeInBytes)
{
    if (sizeInBytes == 0)
    {
        return false;
    }

    if (sizeInBytes <= m_capacity)
    {
        return true;
    }

    Release();
    for (DxrGpuResource& slot : m_slots)
    {
        if (!CreateDxrUploadBuffer(sizeInBytes, slot))
        {
            Release();
            return false;
        }
    }

    m_capacity = m_slots[0].sizeInBytes;
    return true;
}

void TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    const D3D12_RESOURCE_STATES stateBefore,
    const D3D12_RESOURCE_STATES stateAfter)
{
    if (commandList == nullptr || resource == nullptr || stateBefore == stateAfter)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    commandList->ResourceBarrier(1, &barrier);
}

void RecordDxrUavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
{
    if (commandList == nullptr || resource == nullptr)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    commandList->ResourceBarrier(1, &barrier);
}
