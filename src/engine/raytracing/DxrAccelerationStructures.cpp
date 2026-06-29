#include "engine/raytracing/DxrAccelerationStructures.h"

#include "app/scene/Scene.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/Blas.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrInstanceTransform.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"
#include "engine/scene/SceneObject.h"

#include "engine/raytracing/DxrHeaders.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

DxrAccelerationStructures::~DxrAccelerationStructures()
{
    Release();
}

void DxrAccelerationStructures::Release()
{
    m_blasCache.Release();
    m_tlas.Release();
    m_scratchBuffer.Release();
    m_scratchHighWaterMark = 0;
    m_anyBlasBuiltThisFrame = false;
}

bool DxrAccelerationStructures::EnsureScratchBuffer(
    const std::uint64_t requiredBytes,
    std::string& outError)
{
    outError.clear();
    if (requiredBytes == 0)
    {
        return true;
    }

    if (m_scratchBuffer.sizeInBytes >= requiredBytes)
    {
        return true;
    }

    m_scratchBuffer.Release();
    if (!CreateDxrDefaultBuffer(requiredBytes, true, m_scratchBuffer))
    {
        outError = "failed to allocate DXR scratch buffer";
        return false;
    }

    if (requiredBytes > m_scratchHighWaterMark)
    {
        m_scratchHighWaterMark = requiredBytes;
    }

    return true;
}

void DxrAccelerationStructures::EnsureScene(
    const Scene& scene,
    const bool dxrEnabled,
    void* commandList)
{
    const auto buildStart = std::chrono::steady_clock::now();
    m_anyBlasBuiltThisFrame = false;

    if (!GfxContext::Get().IsInitialized())
    {
        m_diagnostics = DxrDiagnostics{};
        m_diagnostics.buildStatus = "SKIPPED (RT off)";
        return;
    }

    if (GfxContext::Get().IsDeviceRemoved())
    {
        Release();
        m_diagnostics = DxrDiagnostics{};
        m_diagnostics.buildStatus = "FAILED: device removed";
        return;
    }

    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled)
    {
        m_diagnostics.blasCount = 0;
        m_diagnostics.tlasInstanceCount = 0;
        m_diagnostics.totalRtTriangles = 0;
        m_diagnostics.asGpuMemoryBytes = 0;
        m_diagnostics.buildStatus = "SKIPPED (RT off)";
        m_diagnostics.lastBuildTimeMs = 0.0;
        return;
    }

    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        m_diagnostics.buildStatus = "FAILED: ID3D12GraphicsCommandList4 unavailable";
        return;
    }

    std::string error;
    std::unordered_set<Mesh*> uniqueMeshes;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh != nullptr)
        {
            uniqueMeshes.insert(mesh);
        }
    }

    std::uint64_t maxScratchBytes = 0;
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 != nullptr)
    {
        for (Mesh* mesh : uniqueMeshes)
        {
            mesh->EnsureGpuResources();
            const std::uint32_t indexCount = static_cast<std::uint32_t>(mesh->GetIndices().size());
            if (indexCount < 3)
            {
                continue;
            }

            const GpuBuffer& vertexBuffer = mesh->GetVertexBuffer();
            const GpuBuffer& indexBuffer = mesh->GetIndexBuffer();
            if (!vertexBuffer.IsValid() || !indexBuffer.IsValid())
            {
                continue;
            }

            const std::uint32_t vertexStride =
                mesh->GetFloatsPerVertex() * static_cast<std::uint32_t>(sizeof(float));

            D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
            geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometryDesc.Triangles.IndexCount = indexCount;
            geometryDesc.Triangles.VertexCount = static_cast<UINT>(mesh->GetPositions().size());
            geometryDesc.Triangles.IndexBuffer =
                static_cast<ID3D12Resource*>(indexBuffer.GetResource())->GetGPUVirtualAddress();
            geometryDesc.Triangles.VertexBuffer.StartAddress =
                static_cast<ID3D12Resource*>(vertexBuffer.GetResource())->GetGPUVirtualAddress();
            geometryDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
            blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            blasInputs.NumDescs = 1;
            blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            blasInputs.pGeometryDescs = &geometryDesc;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &prebuildInfo);
            maxScratchBytes = std::max(maxScratchBytes, prebuildInfo.ScratchDataSizeInBytes);
        }

        const std::uint32_t renderableCount = static_cast<std::uint32_t>(std::count_if(
            objects.begin(),
            objects.end(),
            [](const SceneObject& object) { return object.IsRenderable(); }));
        if (renderableCount > 0)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
            tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            tlasInputs.NumDescs = renderableCount;
            tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);
            maxScratchBytes = std::max(maxScratchBytes, tlasPrebuild.ScratchDataSizeInBytes);
        }
    }

    if (!EnsureScratchBuffer(maxScratchBytes, error))
    {
        m_diagnostics.buildStatus = "FAILED: " + error;
        return;
    }

    {
        SceneRenderTrace::Scope blasScope("dxr-blas-ensure");
        for (Mesh* mesh : uniqueMeshes)
        {
            Blas* existing = m_blasCache.Find(mesh);
            if (existing != nullptr && existing->IsBuilt())
            {
                continue;
            }

            if (!m_blasCache.Ensure(commandList4, mesh, m_scratchBuffer, error))
            {
                m_diagnostics.buildStatus = "FAILED: " + error;
                return;
            }

            m_anyBlasBuiltThisFrame = true;
        }
        blasScope.Success();
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve(objects.size());
    std::uint64_t referencedTriangles = 0;
    std::unordered_set<Mesh*> referencedMeshes;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr)
        {
            continue;
        }

        Blas* blas = m_blasCache.Find(mesh);
        if (blas == nullptr || !blas->IsBuilt())
        {
            continue;
        }

        if (referencedMeshes.insert(mesh).second)
        {
            referencedTriangles += blas->GetTriangleCount();
        }

        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc{};
        WriteD3D12InstanceTransform(
            scene.GetWorldMatrix(static_cast<int>(objectIndex)),
            reinterpret_cast<float*>(instanceDesc.Transform));
        instanceDesc.InstanceID = static_cast<UINT>(objectIndex);
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.InstanceContributionToHitGroupIndex = 0;
        instanceDesc.Flags = object.GetMaterial().IsDoubleSided()
            ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
            : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        instanceDesc.AccelerationStructure = blas->GetGpuVirtualAddress();
        instances.push_back(instanceDesc);
    }

    if (m_anyBlasBuiltThisFrame)
    {
        for (Mesh* mesh : uniqueMeshes)
        {
            Blas* blas = m_blasCache.Find(mesh);
            if (blas != nullptr && blas->IsBuilt() && blas->GetResultResource() != nullptr)
            {
                RecordDxrUavBarrier(
                    static_cast<ID3D12GraphicsCommandList*>(commandList4),
                    blas->GetResultResource());
            }
        }
    }

    {
        SceneRenderTrace::Scope tlasScope("dxr-tlas-build");
        if (!m_tlas.Build(commandList4, instances, m_scratchBuffer, error))
        {
            m_diagnostics.buildStatus = "FAILED: " + error;
            return;
        }
        tlasScope.Success();
    }

    const auto buildEnd = std::chrono::steady_clock::now();
    m_diagnostics.blasCount = m_blasCache.GetCount();
    m_diagnostics.tlasInstanceCount = static_cast<std::uint32_t>(instances.size());
    m_diagnostics.totalRtTriangles = referencedTriangles;
    m_diagnostics.asGpuMemoryBytes =
        m_blasCache.GetTotalMemoryBytes() + m_tlas.GetSizeInBytes() + m_scratchHighWaterMark;
    m_diagnostics.buildStatus = "OK";
    m_diagnostics.lastBuildTimeMs =
        std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();
}
