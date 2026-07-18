#pragma once

#include "engine/raytracing/core/DxrGpuResource.h"
#include "engine/raytracing/core/DxrHeaders.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;
class Tlas
{
public:
    Tlas() = default;
    ~Tlas();

    Tlas(const Tlas&) = delete;
    Tlas& operator=(const Tlas&) = delete;

    bool IsBuilt() const { return m_built; }
    std::uint64_t GetGpuVirtualAddress() const;
    std::uint64_t GetSizeInBytes() const { return m_result.sizeInBytes; }
    std::uint32_t GetInstanceCount() const { return m_instanceCount; }
    ID3D12Resource* GetResultResource() const { return m_result.resource; }

    bool Build(
        ID3D12GraphicsCommandList4* commandList,
        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances,
        DxrGpuResource& scratchBuffer,
        std::string& outError);

    void Release();

private:
    DxrGpuResource m_result{};
    DxrUploadRing m_instanceUploadRing{};
    std::uint32_t m_instanceCount = 0;
    bool m_built = false;
};
