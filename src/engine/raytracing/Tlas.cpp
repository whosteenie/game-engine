#include "engine/raytracing/Tlas.h"

#include "engine/platform/EngineLog.h"
#include "engine/raytracing/DxrHeaders.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

#include <cstring>
#include <sstream>

Tlas::~Tlas()
{
    Release();
}

std::uint64_t Tlas::GetGpuVirtualAddress() const
{
    return m_built ? m_result.GetGpuVirtualAddress() : 0ull;
}

void Tlas::Release()
{
    m_result.Release();
    m_instanceUploadRing.Release();
    m_built = false;
    m_instanceCount = 0;
}

bool Tlas::Build(
    ID3D12GraphicsCommandList4* commandList,
    const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances,
    DxrGpuResource& scratchBuffer,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr)
    {
        outError = "null command list";
        return false;
    }

    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    const std::uint32_t instanceCount = static_cast<std::uint32_t>(instances.size());
    if (instanceCount == 0)
    {
        Release();
        return true;
    }

    const std::uint64_t instanceUploadSize =
        static_cast<std::uint64_t>(instanceCount) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!m_instanceUploadRing.EnsureCapacity(instanceUploadSize))
    {
        outError = "failed to allocate TLAS instance upload buffer";
        return false;
    }

    DxrGpuResource& instanceUpload =
        m_instanceUploadRing.Slot(GfxContext::Get().GetFrameIndex());
    void* mapped = nullptr;
    if (FAILED(instanceUpload.resource->Map(0, nullptr, &mapped)))
    {
        outError = "failed to map TLAS instance upload buffer";
        return false;
    }

    std::memcpy(mapped, instances.data(), static_cast<std::size_t>(instanceUploadSize));
    instanceUpload.resource->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    // The caller skips this build when topology and transforms are unchanged, so
    // path tracing benefits more from optimizing the retained TLAS for traversal.
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasInputs.NumDescs = instanceCount;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = instanceUpload.GetGpuVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        outError = "TLAS prebuild size is zero";
        return false;
    }

    if (scratchBuffer.sizeInBytes < prebuildInfo.ScratchDataSizeInBytes)
    {
        outError = "scratch buffer too small for TLAS build";
        return false;
    }

    if (m_result.sizeInBytes < prebuildInfo.ResultDataMaxSizeInBytes)
    {
        m_result.Release();
        if (!CreateDxrDefaultBuffer(prebuildInfo.ResultDataMaxSizeInBytes, true, m_result))
        {
            outError = "failed to allocate TLAS result buffer";
            return false;
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = tlasInputs;
    buildDesc.ScratchAccelerationStructureData = scratchBuffer.GetGpuVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_result.GetGpuVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_result.resource);

    m_instanceCount = instanceCount;
    m_built = true;

    std::ostringstream logMessage;
    logMessage << "TLAS: instances=" << instanceCount
               << " gpu=" << (prebuildInfo.ResultDataMaxSizeInBytes / 1024u) << "KB";
    EngineLog::Info("dxr-as", logMessage.str());

    return true;
}
