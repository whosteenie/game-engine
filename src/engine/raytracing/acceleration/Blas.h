#pragma once

#include "engine/raytracing/core/DxrGpuResource.h"

#include <cstdint>
#include <string>

class Mesh;

struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;

class Blas
{
public:
    Blas() = default;
    ~Blas();

    Blas(const Blas&) = delete;
    Blas& operator=(const Blas&) = delete;

    bool IsBuilt() const { return m_built; }
    std::uint64_t GetGpuVirtualAddress() const;
    std::uint64_t GetSizeInBytes() const { return m_result.sizeInBytes; }
    ID3D12Resource* GetResultResource() const { return m_result.resource; }
    std::uint32_t GetTriangleCount() const { return m_triangleCount; }
    std::uint32_t GetCachedIndexCount() const { return m_cachedIndexCount; }

    bool Build(
        ID3D12GraphicsCommandList4* commandList,
        Mesh* mesh,
        DxrGpuResource& scratchBuffer,
        std::string& outError);

    void Release();

private:
    DxrGpuResource m_result{};
    std::uint32_t m_triangleCount = 0;
    std::uint32_t m_cachedIndexCount = 0;
    bool m_built = false;
};
