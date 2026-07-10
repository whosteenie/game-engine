#pragma once

#include "engine/raytracing/DxrDispatchBase.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <string>

struct ID3D12StateObject;

// G8 — ReSTIR temporal/spatial raygen PSOs + SBTs (devdoc/dxr/pt/restir-pt.md).
// Buffers live on the path-tracer DxrDispatchContext; this class only owns pipelines.
// No DispatchRays until R2/R3.
class DxrRestirDispatch : public DxrDispatchBase
{
public:
    DxrRestirDispatch() = default;
    ~DxrRestirDispatch();

    DxrRestirDispatch(const DxrRestirDispatch&) = delete;
    DxrRestirDispatch& operator=(const DxrRestirDispatch&) = delete;

    void Release();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return DxrDispatchBase::IsPipelineReady(); }
    bool IsSpatialPipelineReady() const { return m_spatialPipelineReady; }

    ID3D12StateObject* GetStateObject() const { return m_pipeline.GetStateObject(); }
    const ShaderBindingTable& GetTemporalShaderBindingTable() const { return m_shaderBindingTable; }
    const ShaderBindingTable& GetSpatialShaderBindingTable() const { return m_spatialShaderBindingTable; }

private:
    bool EnsurePipelines(std::string& outError);

    ShaderBindingTable m_spatialShaderBindingTable;
    bool m_spatialPipelineReady = false;
};
