#pragma once

#include "engine/raytracing/dispatch/DxrDispatchBase.h"

#include <cstdint>
#include <string>

class DxrAccelerationStructures;

class DxrSmokeDispatch : public DxrDispatchBase
{
public:
    DxrSmokeDispatch() = default;
    ~DxrSmokeDispatch();

    DxrSmokeDispatch(const DxrSmokeDispatch&) = delete;
    DxrSmokeDispatch& operator=(const DxrSmokeDispatch&) = delete;

    void DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        bool dxrEnabled,
        bool smokeDebugMode,
        void* commandList,
        int width,
        int height);

    void Release();
    void ResetProjectResources();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return DxrDispatchBase::IsPipelineReady(); }

    std::uintptr_t GetOutputSrvCpuHandle() const;
    bool HasValidOutput() const;

private:
    bool EnsurePipeline(std::string& outError);
};
