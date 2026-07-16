#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <functional>
#include <string>

struct ID3D12GraphicsCommandList4;
class DxrAccelerationStructures;

enum class DxrDispatchGeometryRequirement
{
    TlasOnly,
    TlasAndGeometryLookup,
};

class DxrDispatchBase
{
public:
    virtual ~DxrDispatchBase() = default;

    void ReleaseCore();
    bool IsPipelineReady() const { return m_pipelineReady; }

protected:
    void ResetDispatchResources();

    using CreatePipelineFn = std::function<bool(DxrPipeline&, std::string&)>;
    using BuildShaderTableFn =
        std::function<bool(ShaderBindingTable&, const DxrPipeline&, std::string&)>;

    bool EnsurePipelineWith(
        const char* traceLabel,
        CreatePipelineFn createPipeline,
        BuildShaderTableFn buildShaderTable,
        std::string& outError);

    ID3D12GraphicsCommandList4* ResolveDispatchCommandList(
        const DxrAccelerationStructures& accelerationStructures,
        bool dxrEnabled,
        int width,
        int height,
        void* commandList,
        DxrDispatchGeometryRequirement geometryRequirement,
        const char* skipBreadcrumbLabel) const;

    DxrPipeline m_pipeline;
    ShaderBindingTable m_shaderBindingTable;
    DxrDispatchContext m_dispatchContext;
    bool m_pipelineReady = false;
};
