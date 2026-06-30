#include "engine/raytracing/ShaderBindingTable.h"

#include "engine/raytracing/DxrTrace.h"

#include <d3d12.h>

#include <cstring>

void ShaderBindingTable::Release()
{
    m_uploadBuffer.Release();
    m_recordStride = 0;
    m_raygenOffset = 0;
    m_missTableOffset = 0;
    m_hitGroupTableOffset = 0;
}

bool ShaderBindingTable::BuildSmokeTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("SBT BuildSmokeTable begin");

    if (stateObjectProperties == nullptr)
    {
        outError = "null state object properties";
        return false;
    }

    m_recordStride = GetShaderRecordStride(0);

    m_raygenOffset = 0;
    m_missTableOffset = AlignShaderTableSize(static_cast<std::uint64_t>(m_recordStride));
    m_hitGroupTableOffset = m_missTableOffset + AlignShaderTableSize(static_cast<std::uint64_t>(m_recordStride));

    const std::uint64_t totalSize = m_hitGroupTableOffset + static_cast<std::uint64_t>(m_recordStride);
    if (!CreateDxrUploadBuffer(totalSize, m_uploadBuffer))
    {
        outError = "failed to allocate shader binding table upload buffer";
        return false;
    }

    void* mapped = nullptr;
    if (FAILED(m_uploadBuffer.resource->Map(0, nullptr, &mapped)))
    {
        outError = "failed to map shader binding table upload buffer";
        Release();
        return false;
    }

    std::memset(mapped, 0, static_cast<std::size_t>(totalSize));

    const void* raygenIdentifier = stateObjectProperties->GetShaderIdentifier(L"SmokeRayGen");
    const void* missIdentifier = stateObjectProperties->GetShaderIdentifier(L"SmokeMiss");
    const void* hitGroupIdentifier = stateObjectProperties->GetShaderIdentifier(L"SmokeHitGroup");
    if (raygenIdentifier == nullptr || missIdentifier == nullptr || hitGroupIdentifier == nullptr)
    {
        m_uploadBuffer.resource->Unmap(0, nullptr);
        outError = "failed to query shader identifiers for smoke SBT";
        Release();
        return false;
    }

    auto* bytes = static_cast<std::uint8_t*>(mapped);
    std::memcpy(bytes + m_raygenOffset, raygenIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(bytes + m_missTableOffset, missIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(bytes + m_hitGroupTableOffset, hitGroupIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    m_uploadBuffer.resource->Unmap(0, nullptr);

    DxrBreadcrumb("SBT BuildSmokeTable ok");
    return true;
}

std::uint64_t ShaderBindingTable::GetRaygenGpuAddress() const
{
    return m_uploadBuffer.GetGpuVirtualAddress() + m_raygenOffset;
}

std::uint64_t ShaderBindingTable::GetMissGpuAddress() const
{
    return m_uploadBuffer.GetGpuVirtualAddress() + m_missTableOffset;
}

std::uint64_t ShaderBindingTable::GetHitGroupGpuAddress() const
{
    return m_uploadBuffer.GetGpuVirtualAddress() + m_hitGroupTableOffset;
}
