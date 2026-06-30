#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <string>

class DxrAccelerationStructures;

class DxrSmokeDispatch
{
public:
    DxrSmokeDispatch() = default;
    ~DxrSmokeDispatch();

    DxrSmokeDispatch(const DxrSmokeDispatch&) = delete;
    DxrSmokeDispatch& operator=(const DxrSmokeDispatch&) = delete;

    void DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        bool dxrEnabled,
        void* commandList,
        int width,
        int height);

    void Release();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return m_pipelineReady; }

    std::uintptr_t GetOutputSrvCpuHandle() const;
    bool HasValidOutput() const;

private:
    bool EnsurePipeline(std::string& outError);

    DxrPipeline m_pipeline;
    ShaderBindingTable m_shaderBindingTable;
    DxrDispatchContext m_dispatchContext;
    bool m_pipelineReady = false;
};
