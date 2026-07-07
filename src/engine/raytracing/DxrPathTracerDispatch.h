#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <string>

class Camera;
class DxrAccelerationStructures;

// Phase P0 — unified path tracer (devdoc/dxr-path-tracing.md).
//
// This is the scaffolding for the megakernel path tracer: it owns the PT RTPSO + SBT and dispatches
// pure camera-ray tracing (no raster/depth dependency), writing a primary-hit world-normal debug
// image. It deliberately reuses the primary-debug global root signature + output textures (via
// DxrDispatchContext::DispatchPrimaryDebug), so P0 adds no new dispatch/output plumbing. P1 grows the
// raygen into the real path integrator (direct lighting), then multi-bounce GI, denoise, etc.
class DxrPathTracerDispatch
{
public:
    DxrPathTracerDispatch() = default;
    ~DxrPathTracerDispatch();

    DxrPathTracerDispatch(const DxrPathTracerDispatch&) = delete;
    DxrPathTracerDispatch& operator=(const DxrPathTracerDispatch&) = delete;

    // Runs when path tracing is the active rendering mode (and DXR is supported/enabled). Traces one
    // camera ray per pixel into the shared primary-debug output textures.
    bool DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        bool dxrEnabled,
        bool pathTracingActive,
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
