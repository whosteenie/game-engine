#pragma once

#include "engine/raytracing/DxrRootSignature.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;
struct ID3D12StateObject;
struct ID3D12RootSignature;

namespace D3D12MA
{
class Allocation;
}

class DxrPipeline;
class ShaderBindingTable;

class DxrDispatchContext
{
public:
    DxrDispatchContext() = default;
    ~DxrDispatchContext();

    DxrDispatchContext(const DxrDispatchContext&) = delete;
    DxrDispatchContext& operator=(const DxrDispatchContext&) = delete;

    bool EnsureOutput(int width, int height, std::string& outError);
    void Release();

    bool DispatchSmoke(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        int width,
        int height,
        const DxrRootSignature::DispatchConstants& constants,
        std::string& outError);

    std::uintptr_t GetOutputSrvCpuHandle() const { return m_outputSrvCpuHandle; }
    int GetOutputWidth() const { return m_outputWidth; }
    int GetOutputHeight() const { return m_outputHeight; }
    ID3D12Resource* GetOutputResource() const { return m_outputResource; }

private:
    bool EnsureConstantBuffer(std::string& outError);
    void WriteConstantBuffer(const DxrRootSignature::DispatchConstants& constants);
    bool CreateTlasSrv(
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        std::string& outError);
    void CreateOutputDescriptors();
    void ReleaseRetiredOutputs();

    struct RetiredOutput
    {
        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uint32_t uavIndex = UINT32_MAX;
    };

    ID3D12Resource* m_outputResource = nullptr;
    D3D12MA::Allocation* m_outputAllocation = nullptr;
    ID3D12Resource* m_constantBufferResource = nullptr;
    D3D12MA::Allocation* m_constantBufferAllocation = nullptr;

    std::uint32_t m_tlasSrvIndex = UINT32_MAX;
    std::uint32_t m_outputUavIndex = UINT32_MAX;
    std::uint32_t m_outputSrvIndex = UINT32_MAX;
    std::uintptr_t m_outputSrvCpuHandle = 0;
    std::uint32_t m_outputResourceState = 0;
    int m_outputWidth = 0;
    int m_outputHeight = 0;

    std::vector<RetiredOutput> m_retiredOutputs;
};
