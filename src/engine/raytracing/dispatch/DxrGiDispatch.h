#pragma once

#include "engine/raytracing/dispatch/DxrDispatchBase.h"
#include "engine/raytracing/denoising/NrdDenoiser.h"

#include <array>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

class Camera;
class DxrAccelerationStructures;

// Phase D9 — one-bounce RT diffuse GI trace + NRD RELAX_DIFFUSE denoise
// (devdoc/dxr/diffuse-gi.md). Owns the GI RTPSO, SBT, quality-scaled RGBA16F radiance+hitDist
// output, and a dedicated RELAX_DIFFUSE denoiser instance (SEPARATE from the reflections RELAX
// instance — the two guides/history live in independent texture sets). Structurally a copy of
// DxrReflectionsDispatch: same inputs, same reflection global root signature + constants struct.
class DxrGiDispatch : public DxrDispatchBase
{
public:
    // G-buffer / environment SRV CPU handles (shader-visible SRV heap). Mirrors the reflection
    // dispatch inputs; the GGX-specific ones simply go unused by the diffuse trace.
    struct FrameInputs
    {
        std::uintptr_t depthSrvCpuHandle = 0;
        std::uintptr_t normalSrvCpuHandle = 0;    // RT2 shading normal
        std::uintptr_t material0SrvCpuHandle = 0; // RT5 albedo+roughness
        std::uintptr_t directSrvCpuHandle = 0;    // RT0 (unused; bound for parity)
        std::uintptr_t sunShadowSrvCpuHandle = 0; // RT3 (unused; bound for parity)
        std::uintptr_t indirectSrvCpuHandle = 0;  // RT1 (unused; bound for parity)
        std::uintptr_t prefilterSrvCpuHandle = 0; // IBL prefiltered env cube (miss radiance)
        std::uintptr_t velocitySrvCpuHandle = 0;  // RT4 motion NDC (NRD guide source)
        float environmentIntensity = 1.0f;
        float maxReflectionLod = 4.0f;
        // In-hit analytic shading inputs (shared with reflections).
        std::uint32_t materialSrvIndex = UINT32_MAX; // per-object material table (t12)
        glm::vec3 sunDirection = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor = glm::vec3(1.0f);
        float sunIntensity = 0.0f;
        std::array<glm::vec4, 9> irradianceSh9{}; // L2 SH diffuse irradiance
    };

    DxrGiDispatch() = default;
    ~DxrGiDispatch();

    DxrGiDispatch(const DxrGiDispatch&) = delete;
    DxrGiDispatch& operator=(const DxrGiDispatch&) = delete;

    bool DispatchIfEnabled(
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        bool dxrEnabled,
        bool giEnabled,
        bool giDebugViewActive,
        void* commandList,
        const FrameInputs& frameInputs,
        int outputWidth,
        int outputHeight,
        int gbufferWidth,
        int gbufferHeight,
        float maxTraceDistance,
        int samplesPerPixel,
        bool denoiseEnabled,
        float temporalBlend,
        int atrousIterations,
        bool antiFirefly);

    void Release();
    void ResetProjectResources();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return DxrDispatchBase::IsPipelineReady(); }
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }

    std::uintptr_t GetGiOutputSrvCpuHandle() const;
    // NRD-denoised output; 0 until the denoiser has run this frame.
    std::uintptr_t GetDenoisedSrvCpuHandle() const;
    bool HasValidOutput() const;
    bool DenoisedThisFrame() const { return m_denoisedThisFrame; }
    // dispatchSize / textureSize — consumers must scale UVs by this (the texture can be larger
    // than the last dispatch when quality shrinks or viewport sizes differ).
    float GetOutputUvScaleX() const;
    float GetOutputUvScaleY() const;

private:
    bool EnsurePipeline(std::string& outError);

    NrdDenoiser m_denoiser;
    glm::mat4 m_prevViewToClip{1.0f};
    glm::mat4 m_prevWorldToView{1.0f};
    glm::vec2 m_prevJitterUv{0.0f};
    std::uint32_t m_frameIndex = 0;
    bool m_nrdHistoryValid = false;
    bool m_dispatchedThisFrame = false;
    bool m_denoisedThisFrame = false;
};
