#pragma once

#include "engine/rhi/GfxContext.h"

#include <array>
#include <cstdint>

#include <d3d12.h>

namespace D3D12MA
{
class Allocation;
}

struct ID3D12Resource;

struct DxrGpuResource
{
    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    std::uint64_t sizeInBytes = 0;

    void Release();
    std::uint64_t GetGpuVirtualAddress() const;
};

bool CreateDxrDefaultBuffer(
    std::uint64_t sizeInBytes,
    bool allowUnorderedAccess,
    DxrGpuResource& outResource,
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

bool CreateDxrScratchBuffer(std::uint64_t sizeInBytes, DxrGpuResource& outResource);

bool CreateDxrUploadBuffer(std::uint64_t sizeInBytes, DxrGpuResource& outResource);

// CRASH-02: one upload buffer per frame-in-flight so CPU writes cannot race GPU reads.
class DxrUploadRing
{
public:
    void Release();
    DxrGpuResource& Slot(std::uint32_t frameIndex);
    const DxrGpuResource& Slot(std::uint32_t frameIndex) const;
    bool EnsureCapacity(std::uint64_t sizeInBytes);
    std::uint64_t GetCapacity() const { return m_capacity; }

private:
    std::array<DxrGpuResource, GfxContext::FrameCount> m_slots{};
    std::uint64_t m_capacity = 0;
};

// DEFAULT-heap buffers in SRV read state; paired with DxrUploadRing staging (DXR-05).
class DxrSrvBufferRing
{
public:
    void Release();
    DxrGpuResource& Slot(std::uint32_t frameIndex);
    const DxrGpuResource& Slot(std::uint32_t frameIndex) const;
    bool EnsureCapacity(std::uint64_t sizeInBytes);
    std::uint64_t GetCapacity() const { return m_capacity; }

private:
    std::array<DxrGpuResource, GfxContext::FrameCount> m_slots{};
    std::uint64_t m_capacity = 0;
};

void CopyDxrUploadToSrvBuffer(
    ID3D12GraphicsCommandList* commandList,
    const DxrGpuResource& upload,
    DxrGpuResource& srvBuffer,
    std::uint64_t byteSize);

void TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter);

void RecordDxrUavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource);
