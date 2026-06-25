#include "engine/rhi/d3d12/GpuBuffer.h"

#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstring>
#include <stdexcept>

namespace
{
    std::uint64_t AlignBufferSize(const std::uint32_t byteSize)
    {
        return (static_cast<std::uint64_t>(byteSize) + 255ull) & ~255ull;
    }

    D3D12_RESOURCE_STATES ResourceStateForType(const GpuBuffer::Type type)
    {
        return type == GpuBuffer::Type::Index
            ? D3D12_RESOURCE_STATE_INDEX_BUFFER
            : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
}

GpuBuffer::~GpuBuffer()
{
    Destroy();
}

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
    : m_resource(other.m_resource),
      m_allocation(other.m_allocation),
      m_type(other.m_type),
      m_byteSize(other.m_byteSize)
{
    other.m_resource = nullptr;
    other.m_allocation = nullptr;
    other.m_byteSize = 0;
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept
{
    if (this != &other)
    {
        Destroy();
        m_resource = other.m_resource;
        m_allocation = other.m_allocation;
        m_type = other.m_type;
        m_byteSize = other.m_byteSize;
        other.m_resource = nullptr;
        other.m_allocation = nullptr;
        other.m_byteSize = 0;
    }

    return *this;
}

void GpuBuffer::Create(const Type type, const void* data, const std::uint32_t byteSize)
{
    Destroy();
    if (byteSize == 0 || data == nullptr)
    {
        return;
    }

    if (!GfxContext::Get().IsInitialized())
    {
        throw std::runtime_error("Failed to create GPU buffer: graphics context is not initialized");
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    const std::uint64_t alignedSize = AlignBufferSize(byteSize);

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

    D3D12MA::ALLOCATION_DESC defaultAllocationDesc{};
    defaultAllocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    const HRESULT createDefaultResult = allocator->CreateResource(
        &defaultAllocationDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        &allocation,
        IID_PPV_ARGS(&resource));
    if (FAILED(createDefaultResult))
    {
        throw std::runtime_error(
            std::string("Failed to create GPU buffer (default heap) (HRESULT=0x")
            + std::to_string(static_cast<unsigned long>(createDefaultResult)) + ")");
    }

    D3D12MA::ALLOCATION_DESC uploadAllocationDesc{};
    uploadAllocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ID3D12Resource* uploadResource = nullptr;
    D3D12MA::Allocation* uploadAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &uploadAllocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &uploadAllocation,
            IID_PPV_ARGS(&uploadResource))))
    {
        allocation->Release();
        resource->Release();
        throw std::runtime_error("Failed to create GPU buffer (upload staging)");
    }

    void* mapped = nullptr;
    if (FAILED(uploadResource->Map(0, nullptr, &mapped)))
    {
        uploadAllocation->Release();
        uploadResource->Release();
        allocation->Release();
        resource->Release();
        throw std::runtime_error("Failed to map GPU buffer upload staging");
    }

    std::memcpy(mapped, data, byteSize);
    uploadResource->Unmap(0, nullptr);

    ID3D12Resource* destinationResource = resource;
    const std::uint64_t copySize = alignedSize;
    const D3D12_RESOURCE_STATES finalState = ResourceStateForType(type);
    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);
        commandList->CopyBufferRegion(destinationResource, 0, uploadResource, 0, copySize);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destinationResource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        commandList->ResourceBarrier(1, &barrier);
    });

    uploadAllocation->Release();
    uploadResource->Release();

    m_resource = resource;
    m_allocation = allocation;
    m_type = type;
    m_byteSize = byteSize;
    (void)device;
}

void GpuBuffer::CreateUpload(const Type type, const void* data, const std::uint32_t byteSize)
{
    Destroy();
    if (byteSize == 0 || data == nullptr)
    {
        return;
    }

    if (!GfxContext::Get().IsInitialized())
    {
        throw std::runtime_error("Failed to create GPU upload buffer: graphics context is not initialized");
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    const std::uint64_t alignedSize = AlignBufferSize(byteSize);

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
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error("Failed to create GPU upload buffer");
    }

    void* mapped = nullptr;
    if (FAILED(resource->Map(0, nullptr, &mapped)))
    {
        allocation->Release();
        resource->Release();
        throw std::runtime_error("Failed to map GPU upload buffer");
    }

    std::memcpy(mapped, data, byteSize);
    resource->Unmap(0, nullptr);

    m_resource = resource;
    m_allocation = allocation;
    m_type = type;
    m_byteSize = byteSize;
}

void GpuBuffer::Destroy()
{
    if (m_allocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_allocation)->Release();
        m_allocation = nullptr;
    }

    m_resource = nullptr;
    m_byteSize = 0;
}

void GpuBuffer::BindVertex(const std::uint32_t slot, const std::uint32_t stride) const
{
    BindVertexToCommandList(GfxContext::Get().GetCommandList(), slot, stride);
}

void GpuBuffer::BindVertexToCommandList(
    void* commandList,
    const std::uint32_t slot,
    const std::uint32_t stride) const
{
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(commandList);
    const D3D12_VERTEX_BUFFER_VIEW view{
        static_cast<ID3D12Resource*>(m_resource)->GetGPUVirtualAddress(),
        m_byteSize,
        stride};
    d3dCommandList->IASetVertexBuffers(slot, 1, &view);
}

void GpuBuffer::BindIndex() const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    const D3D12_INDEX_BUFFER_VIEW view{
        static_cast<ID3D12Resource*>(m_resource)->GetGPUVirtualAddress(),
        m_byteSize,
        DXGI_FORMAT_R32_UINT};
    commandList->IASetIndexBuffer(&view);
}
