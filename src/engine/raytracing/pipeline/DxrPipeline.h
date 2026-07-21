#pragma once

#include <cstdint>
#include <string>

struct ID3D12RootSignature;
struct ID3D12StateObject;
struct ID3D12StateObjectProperties;

class DxrPipeline
{
public:
    struct PathTracerPipelineStatus
    {
        const char* compilerLibrary = "not_attempted";
        const char* rtpso = "not_attempted";
    };

    DxrPipeline() = default;
    ~DxrPipeline();

    DxrPipeline(const DxrPipeline&) = delete;
    DxrPipeline& operator=(const DxrPipeline&) = delete;

    bool CreateSmokePipeline(std::string& outError);
    bool CreatePrimaryDebugPipeline(std::string& outError);
    // Phase P0 — unified path tracer RTPSO (devdoc/dxr/path-tracing.md). Reuses the primary-debug
    // global/local root signatures; only the library + export names differ.
    bool CreatePathTracerPipeline(
        std::string& outError,
        bool diagnosticPermutation = false,
        bool serPermutation = false);
    bool CreateReflectionsPipeline(std::string& outError);
    bool CreateShadowsPipeline(std::string& outError);
    bool CreateGiPipeline(std::string& outError);
    // G8 — ReSTIR temporal + spatial raygen stubs in one RTPSO (devdoc/dxr/pt/restir-pt.md).
    bool CreateRestirPipeline(std::string& outError);
    void Release();

    ID3D12StateObject* GetStateObject() const { return m_stateObject; }
    ID3D12StateObjectProperties* GetProperties() const { return m_stateObjectProperties; }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature; }
    const PathTracerPipelineStatus& GetPathTracerPipelineStatus() const { return m_pathTracerPipelineStatus; }

private:
    ID3D12StateObject* m_stateObject = nullptr;
    ID3D12StateObjectProperties* m_stateObjectProperties = nullptr;
    ID3D12RootSignature* m_globalRootSignature = nullptr;
    PathTracerPipelineStatus m_pathTracerPipelineStatus{};
};
