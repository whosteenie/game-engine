#pragma once

#include "engine/raytracing/acceleration/Blas.h"
#include "engine/raytracing/core/DxrGpuResource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class Mesh;

struct ID3D12GraphicsCommandList4;
class BlasCache
{
public:
    std::uint32_t GetCount() const { return static_cast<std::uint32_t>(m_entries.size()); }
    std::uint64_t GetTotalMemoryBytes() const { return m_totalMemoryBytes; }
    std::uint64_t GetReferencedTriangleCount() const { return m_referencedTriangleCount; }

    Blas* Find(Mesh* mesh) const;
    bool Ensure(
        ID3D12GraphicsCommandList4* commandList,
        Mesh* mesh,
        DxrGpuResource& scratchBuffer,
        std::string& outError);

    void Release();
    void InvalidateMesh(Mesh* mesh);

private:
    struct Entry
    {
        std::unique_ptr<Blas> blas;
        std::uint32_t cachedIndexCount = 0;
    };

    std::unordered_map<Mesh*, Entry> m_entries;
    std::uint64_t m_totalMemoryBytes = 0;
    std::uint64_t m_referencedTriangleCount = 0;

    void RecomputeMemoryBytes();
};
