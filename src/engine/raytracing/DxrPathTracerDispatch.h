#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

class Camera;
class DxrAccelerationStructures;

// Phase P0/P1/P2 — unified path tracer (devdoc/dxr-path-tracing.md).
//
// P2: megakernel integrator with multi-bounce BRDF sampling, sun NEE, Russian roulette, and
// firefly clamp. Reuses the reflection root signature + primary output textures.
class DxrPathTracerDispatch
{
public:
    struct FrameInputs
    {
        std::uintptr_t depthSrvCpuHandle = 0;
        std::uintptr_t normalSrvCpuHandle = 0;
        std::uintptr_t material0SrvCpuHandle = 0;
        std::uintptr_t directSrvCpuHandle = 0;
        std::uintptr_t sunShadowSrvCpuHandle = 0;
        std::uintptr_t indirectSrvCpuHandle = 0;
        std::uintptr_t prefilterSrvCpuHandle = 0;
        std::uintptr_t velocitySrvCpuHandle = 0;
        std::uint32_t materialSrvIndex = UINT32_MAX;
        float environmentIntensity = 1.0f;
        float maxReflectionLod = 4.0f;
        glm::vec3 sunDirection = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor = glm::vec3(1.0f);
        float sunIntensity = 0.0f;
    };

    DxrPathTracerDispatch() = default;
    ~DxrPathTracerDispatch();

    DxrPathTracerDispatch(const DxrPathTracerDispatch&) = delete;
    DxrPathTracerDispatch& operator=(const DxrPathTracerDispatch&) = delete;

    // Runs when path tracing is the active rendering mode (and DXR is supported/enabled).
    bool DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        bool dxrEnabled,
        bool pathTracingActive,
        void* commandList,
        const FrameInputs& frameInputs,
        int width,
        int height,
        int gbufferWidth,
        int gbufferHeight,
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
    std::uint32_t m_frameIndex = 0;
};
