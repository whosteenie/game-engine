#pragma once

#include "engine/raytracing/core/DxrGpuResource.h"
#include "engine/raytracing/core/DxrHeaders.h"

#include <cstdint>
#include <string>

struct ID3D12StateObjectProperties;

class ShaderBindingTable
{
public:
    static std::uint32_t GetShaderRecordStride(std::uint32_t localRootArgumentSize = 0)
    {
        const std::uint64_t rawSize =
            static_cast<std::uint64_t>(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) + localRootArgumentSize;
        return static_cast<std::uint32_t>(AlignShaderRecordSize(rawSize));
    }

    static std::uint64_t AlignShaderRecordSize(std::uint64_t sizeInBytes)
    {
        return AlignUp(sizeInBytes, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    }

    static std::uint64_t AlignShaderTableSize(std::uint64_t sizeInBytes)
    {
        return AlignUp(sizeInBytes, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    }

    bool BuildSmokeTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildPrimaryDebugTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildPathTracerTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildReflectionTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildShadowTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildGiTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildRestirTemporalTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    bool BuildRestirGiBoilingFilterTable(
        ID3D12StateObjectProperties* stateObjectProperties,
        std::string& outError);
    bool BuildRestirSpatialTable(ID3D12StateObjectProperties* stateObjectProperties, std::string& outError);
    void Release();

    std::uint64_t GetRaygenGpuAddress() const;
    std::uint64_t GetMissGpuAddress() const;
    std::uint64_t GetHitGroupGpuAddress() const;
    std::uint32_t GetMissRecordStride() const { return m_recordStride; }
    std::uint32_t GetHitGroupRecordStride() const { return m_recordStride; }

private:
    static std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
    {
        if (alignment == 0)
        {
            return value;
        }

        return (value + alignment - 1ull) & ~(alignment - 1ull);
    }

    DxrGpuResource m_uploadBuffer{};
    std::uint32_t m_recordStride = 0;
    std::uint64_t m_raygenOffset = 0;
    std::uint64_t m_missTableOffset = 0;
    std::uint64_t m_hitGroupTableOffset = 0;
};
