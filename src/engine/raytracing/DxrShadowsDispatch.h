#pragma once

#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/NrdShadowDenoiser.h"
#include "engine/raytracing/ShaderBindingTable.h"

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

class Camera;
class DxrAccelerationStructures;

// Phase D8 — RT soft directional (sun) shadow trace + NRD SIGMA_SHADOW denoise
// (devdoc/dxr-shadows.md). Owns the shadows RTPSO, SBT, full-res penumbra output, and a
// dedicated SIGMA denoiser instance (separate from the reflections RELAX instance).
class DxrShadowsDispatch
{
public:
    struct FrameInputs
    {
        std::uintptr_t depthSrvCpuHandle = 0;
        std::uintptr_t normalSrvCpuHandle = 0;    // RT2 shading normal
        std::uintptr_t material0SrvCpuHandle = 0; // RT5 roughness in .a
        std::uintptr_t velocitySrvCpuHandle = 0;  // RT4 motion NDC (NRD guide source)
        glm::vec3 sunDirection = glm::vec3(0.0f, 1.0f, 0.0f); // TOWARD the light (normalized)
        float sunAngularRadiusDegrees = 0.27f;
    };

    DxrShadowsDispatch() = default;
    ~DxrShadowsDispatch();

    DxrShadowsDispatch(const DxrShadowsDispatch&) = delete;
    DxrShadowsDispatch& operator=(const DxrShadowsDispatch&) = delete;

    bool DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        bool dxrEnabled,
        bool shadowsEnabled,
        bool shadowDebugViewActive,
        void* commandList,
        const FrameInputs& frameInputs,
        int width,
        int height,
        int gbufferWidth,
        int gbufferHeight,
        float maxTraceDistance,
        bool denoiseEnabled);

    void Release();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return m_pipelineReady; }
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }
    bool DenoisedThisFrame() const { return m_denoisedThisFrame; }

    // Raw 1-spp penumbra mask (NRD input); 0 until a trace has run.
    std::uintptr_t GetPenumbraSrvCpuHandle() const;
    // SIGMA-denoised OUT_SHADOW_TRANSLUCENCY; 0 until the denoiser has run this frame.
    std::uintptr_t GetDenoisedSrvCpuHandle() const;
    bool HasValidOutput() const;
    // dispatchSize / textureSize — consumers scale UVs by this (kept-alive larger allocations).
    float GetOutputUvScaleX() const;
    float GetOutputUvScaleY() const;

private:
    bool EnsurePipeline(std::string& outError);

    DxrPipeline m_pipeline;
    ShaderBindingTable m_shaderBindingTable;
    DxrDispatchContext m_dispatchContext;
    NrdShadowDenoiser m_denoiser;
    glm::mat4 m_prevViewToClip{1.0f};
    glm::mat4 m_prevWorldToView{1.0f};
    glm::vec2 m_prevJitterUv{0.0f};
    std::uint32_t m_frameIndex = 0;
    bool m_nrdHistoryValid = false;
    bool m_pipelineReady = false;
    bool m_dispatchedThisFrame = false;
    bool m_denoisedThisFrame = false;
};
