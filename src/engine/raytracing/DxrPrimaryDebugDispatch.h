#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <string>

class Camera;
class DxrAccelerationStructures;

class DxrPrimaryDebugDispatch
{
public:
    DxrPrimaryDebugDispatch() = default;
    ~DxrPrimaryDebugDispatch();

    DxrPrimaryDebugDispatch(const DxrPrimaryDebugDispatch&) = delete;
    DxrPrimaryDebugDispatch& operator=(const DxrPrimaryDebugDispatch&) = delete;

    bool DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        bool dxrEnabled,
        bool debugTraceEnabled,
        bool primaryDebugViewActive,
        void* commandList,
        std::uintptr_t depthSrvCpuHandle,
        int width,
        int height,
        float maxTraceDistance);

    void Release();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return m_pipelineReady; }
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }

    std::uintptr_t GetPrimaryOutputSrvCpuHandle() const;
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle() const;
    bool HasValidOutput() const;

private:
    bool EnsurePipeline(std::string& outError);

    DxrPipeline m_pipeline;
    ShaderBindingTable m_shaderBindingTable;
    DxrDispatchContext m_dispatchContext;
    bool m_pipelineReady = false;
    bool m_dispatchedThisFrame = false;
};
