#pragma once

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

void TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter);

void RecordDxrUavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource);
