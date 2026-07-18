#include "engine/raytracing/pipeline/ShaderBindingTable.h"

#include <d3d12.h>

#include <cstdlib>
#include <iostream>

void RunDxrShaderInfrastructureTests(int& failures)
{
    {
        const std::uint32_t stride = ShaderBindingTable::GetShaderRecordStride(0);
        if (stride < D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES)
        {
            std::cerr << "FAIL: DxrSbtRecordStride below shader identifier size\n";
            ++failures;
        }

        if (stride % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT != 0)
        {
            std::cerr << "FAIL: DxrSbtRecordStride not aligned to shader record alignment\n";
            ++failures;
        }
    }

    {
        const std::uint64_t raygenOffset = 0;
        const std::uint32_t recordStride = ShaderBindingTable::GetShaderRecordStride(0);
        const std::uint64_t missTableOffset =
            ShaderBindingTable::AlignShaderTableSize(static_cast<std::uint64_t>(recordStride));
        const std::uint64_t hitGroupTableOffset =
            missTableOffset + ShaderBindingTable::AlignShaderTableSize(static_cast<std::uint64_t>(recordStride));

        if (missTableOffset % D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT != 0)
        {
            std::cerr << "FAIL: DxrSbtMissTableOffset not 64-byte aligned\n";
            ++failures;
        }

        if (hitGroupTableOffset % D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT != 0)
        {
            std::cerr << "FAIL: DxrSbtHitGroupTableOffset not 64-byte aligned\n";
            ++failures;
        }

        if (raygenOffset + recordStride > missTableOffset)
        {
            std::cerr << "FAIL: DxrSbtRaygen overlaps miss table\n";
            ++failures;
        }
    }
}
