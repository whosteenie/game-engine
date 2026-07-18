#pragma once

#include "engine/raytracing/dispatch/DxrDispatchBase.h"

#include <cstdint>
#include <string>

class Camera;
class DxrAccelerationStructures;

class DxrPrimaryDebugDispatch : public DxrDispatchBase
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
    void ResetProjectResources();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return DxrDispatchBase::IsPipelineReady(); }
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }

    std::uintptr_t GetPrimaryOutputSrvCpuHandle() const;
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle() const;
    bool HasValidOutput() const;

private:
    bool EnsurePipeline(std::string& outError);

    bool m_dispatchedThisFrame = false;
};
