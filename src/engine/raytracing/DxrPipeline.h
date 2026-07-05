#pragma once

#include <cstdint>
#include <string>

struct ID3D12RootSignature;
struct ID3D12StateObject;
struct ID3D12StateObjectProperties;

class DxrPipeline
{
public:
    DxrPipeline() = default;
    ~DxrPipeline();

    DxrPipeline(const DxrPipeline&) = delete;
    DxrPipeline& operator=(const DxrPipeline&) = delete;

    bool CreateSmokePipeline(std::string& outError);
    bool CreatePrimaryDebugPipeline(std::string& outError);
    bool CreateReflectionsPipeline(std::string& outError);
    void Release();

    ID3D12StateObject* GetStateObject() const { return m_stateObject; }
    ID3D12StateObjectProperties* GetProperties() const { return m_stateObjectProperties; }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature; }

private:
    ID3D12StateObject* m_stateObject = nullptr;
    ID3D12StateObjectProperties* m_stateObjectProperties = nullptr;
    ID3D12RootSignature* m_globalRootSignature = nullptr;
};
