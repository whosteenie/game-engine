#include "engine/raytracing/Blas.h"

#include "engine/platform/EngineLog.h"
#include "engine/raytracing/DxrHeaders.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <d3d12.h>

#include <sstream>

Blas::~Blas()
{
    Release();
}

std::uint64_t Blas::GetGpuVirtualAddress() const
{
    return m_built ? m_result.GetGpuVirtualAddress() : 0ull;
}

void Blas::Release()
{
    m_result.Release();
    m_built = false;
    m_triangleCount = 0;
    m_cachedIndexCount = 0;
}

bool Blas::Build(
    ID3D12GraphicsCommandList4* commandList,
    Mesh* mesh,
    DxrGpuResource& scratchBuffer,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || mesh == nullptr)
    {
        outError = "null command list or mesh";
        return false;
    }

    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    mesh->EnsureGpuResources();

    const std::vector<unsigned int>& indices = mesh->GetIndices();
    const std::uint32_t indexCount = static_cast<std::uint32_t>(indices.size());
    if (indexCount < 3)
    {
        outError = "mesh has no triangles";
        return false;
    }

    const GpuBuffer& vertexBuffer = mesh->GetVertexBuffer();
    const GpuBuffer& indexBuffer = mesh->GetIndexBuffer();
    if (!vertexBuffer.IsValid() || !indexBuffer.IsValid())
    {
        outError = "mesh GPU buffers unavailable";
        return false;
    }

    const std::uint32_t triangleCount = indexCount / 3u;
    const std::uint32_t vertexStride =
        mesh->GetFloatsPerVertex() * static_cast<std::uint32_t>(sizeof(float));

    auto* vertexResource = static_cast<ID3D12Resource*>(vertexBuffer.GetResource());
    auto* indexResource = static_cast<ID3D12Resource*>(indexBuffer.GetResource());

    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Triangles.Transform3x4 = 0;
    geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometryDesc.Triangles.IndexCount = indexCount;
    geometryDesc.Triangles.VertexCount = static_cast<UINT>(mesh->GetPositions().size());
    geometryDesc.Triangles.IndexBuffer =
        indexResource->GetGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StartAddress =
        vertexResource->GetGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs = 1;
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = &geometryDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        outError = "BLAS prebuild size is zero";
        return false;
    }

    if (scratchBuffer.sizeInBytes < prebuildInfo.ScratchDataSizeInBytes)
    {
        outError = "scratch buffer too small for BLAS build";
        return false;
    }

    Release();
    if (!CreateDxrDefaultBuffer(prebuildInfo.ResultDataMaxSizeInBytes, true, m_result))
    {
        outError = "failed to allocate BLAS result buffer";
        return false;
    }

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        vertexResource,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        indexResource,
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = blasInputs;
    buildDesc.ScratchAccelerationStructureData = scratchBuffer.GetGpuVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_result.GetGpuVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_result.resource);
    // DXR-02: consecutive builds reuse the same scratch buffer in one command list. Per spec,
    // scratch reuse requires a UAV barrier covering the scratch between builds — the barrier on
    // the result above does not order scratch access and builds may otherwise overlap on the GPU.
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), scratchBuffer.resource);

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        vertexResource,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        indexResource,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    m_triangleCount = triangleCount;
    m_cachedIndexCount = indexCount;
    m_built = true;

    std::ostringstream logMessage;
    logMessage << "BLAS built: mesh=0x" << std::hex << reinterpret_cast<std::uintptr_t>(mesh)
               << std::dec << " triangles=" << triangleCount
               << " size=" << (prebuildInfo.ResultDataMaxSizeInBytes / 1024u) << "KB";
    EngineLog::Info("dxr-as", logMessage.str());

    return true;
}
