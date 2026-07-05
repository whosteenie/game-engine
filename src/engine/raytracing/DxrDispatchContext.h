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

    bool DispatchPrimaryDebug(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        std::uintptr_t depthSrvCpuHandle,
        std::uint32_t geometryLookupSrvIndex,
        std::uint32_t sceneVertexFloatsSrvIndex,
        std::uint32_t sceneIndicesSrvIndex,
        int width,
        int height,
        const DxrRootSignature::PrimaryDispatchConstants& constants,
        std::string& outError);

    std::uintptr_t GetOutputSrvCpuHandle() const { return m_outputSrvCpuHandle; }
    std::uintptr_t GetPrimaryOutputSrvCpuHandle() const { return m_primaryOutputSrvCpuHandle; }
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle() const { return m_primaryMetadataSrvCpuHandle; }
    int GetOutputWidth() const { return m_outputWidth; }
    int GetOutputHeight() const { return m_outputHeight; }
    ID3D12Resource* GetOutputResource() const { return m_outputResource; }

private:
    bool CreateTlasSrv(
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        std::string& outError);
    void CreateOutputDescriptors();
    bool EnsurePrimaryOutput(int width, int height, std::string& outError);
    void CreatePrimaryOutputDescriptors();
    void ReleaseRetiredOutputs();
    void ReleaseRetiredPrimaryOutputs();
    std::uint32_t DepthSrvIndexFromCpuHandle(std::uintptr_t depthSrvCpuHandle) const;

    struct RetiredOutput
    {
        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uint32_t uavIndex = UINT32_MAX;
    };

    ID3D12Resource* m_outputResource = nullptr;
    D3D12MA::Allocation* m_outputAllocation = nullptr;

    std::uint32_t m_tlasSrvIndex = UINT32_MAX;
    std::uint32_t m_outputUavIndex = UINT32_MAX;
    std::uint32_t m_outputSrvIndex = UINT32_MAX;
    std::uintptr_t m_outputSrvCpuHandle = 0;
    std::uint32_t m_outputResourceState = 0;
    int m_outputWidth = 0;
    int m_outputHeight = 0;

    std::vector<RetiredOutput> m_retiredOutputs;

    ID3D12Resource* m_primaryOutputResource = nullptr;
    D3D12MA::Allocation* m_primaryOutputAllocation = nullptr;
    ID3D12Resource* m_primaryMetadataResource = nullptr;
    D3D12MA::Allocation* m_primaryMetadataAllocation = nullptr;

    std::uint32_t m_primaryOutputUavIndex = UINT32_MAX;
    std::uint32_t m_primaryOutputSrvIndex = UINT32_MAX;
    std::uint32_t m_primaryMetadataUavIndex = UINT32_MAX;
    std::uint32_t m_primaryMetadataSrvIndex = UINT32_MAX;
    std::uintptr_t m_primaryOutputSrvCpuHandle = 0;
    std::uintptr_t m_primaryMetadataSrvCpuHandle = 0;
    std::uint32_t m_primaryOutputResourceState = 0;
    std::uint32_t m_primaryMetadataResourceState = 0;
    int m_primaryOutputWidth = 0;
    int m_primaryOutputHeight = 0;

    std::vector<RetiredOutput> m_retiredPrimaryOutputs;
    std::vector<RetiredOutput> m_retiredPrimaryMetadata;
};
