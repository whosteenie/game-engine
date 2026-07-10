#include "engine/raytracing/BlasCache.h"

#include "engine/raytracing/Blas.h"
#include "engine/rendering/Mesh.h"

Blas* BlasCache::Find(Mesh* mesh) const
{
    const auto iterator = m_entries.find(mesh);
    if (iterator == m_entries.end())
    {
        return nullptr;
    }

    return iterator->second.blas.get();
}

bool BlasCache::Ensure(
    ID3D12GraphicsCommandList4* commandList,
    Mesh* mesh,
    DxrGpuResource& scratchBuffer,
    std::string& outError)
{
    outError.clear();
    if (mesh == nullptr)
    {
        outError = "null mesh";
        return false;
    }

    const std::uint32_t indexCount = static_cast<std::uint32_t>(mesh->GetIndices().size());
    if (indexCount < 3)
    {
        return true;
    }

    auto iterator = m_entries.find(mesh);
    if (iterator != m_entries.end())
    {
        Blas* blas = iterator->second.blas.get();
        if (blas->IsBuilt() && blas->GetCachedIndexCount() == indexCount)
        {
            return true;
        }

        iterator->second.blas = std::make_unique<Blas>();
        blas = iterator->second.blas.get();
        if (!blas->Build(commandList, mesh, scratchBuffer, outError))
        {
            m_entries.erase(iterator);
            RecomputeMemoryBytes();
            return false;
        }

        iterator->second.cachedIndexCount = indexCount;
        RecomputeMemoryBytes();
        return true;
    }

    Entry entry{};
    entry.blas = std::make_unique<Blas>();
    if (!entry.blas->Build(commandList, mesh, scratchBuffer, outError))
    {
        return false;
    }

    entry.cachedIndexCount = indexCount;
    m_entries.emplace(mesh, std::move(entry));
    RecomputeMemoryBytes();
    return true;
}

void BlasCache::Release()
{
    m_entries.clear();
    m_totalMemoryBytes = 0;
    m_referencedTriangleCount = 0;
}

void BlasCache::InvalidateMesh(Mesh* mesh)
{
    m_entries.erase(mesh);
    RecomputeMemoryBytes();
}

void BlasCache::RecomputeMemoryBytes()
{
    m_totalMemoryBytes = 0;
    for (const auto& pair : m_entries)
    {
        if (pair.second.blas != nullptr && pair.second.blas->IsBuilt())
        {
            m_totalMemoryBytes += pair.second.blas->GetSizeInBytes();
        }
    }
}
