#pragma once

#include "engine/raytracing/DxrDispatchBase.h"

#include <array>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

class Camera;
class DxrAccelerationStructures;
class DxrRestirDispatch;
class DxrRestirDispatch;

// Phase P0/P1/P2 — unified path tracer (devdoc/dxr/path-tracing.md).
//
// P2: megakernel integrator with multi-bounce BRDF sampling, sun NEE, Russian roulette, and
// firefly clamp. Reuses the reflection root signature + primary output textures.
class DxrPathTracerDispatch : public DxrDispatchBase
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
        float sunAngularRadiusDegrees = 0.27f;
        std::array<glm::vec4, 9> irradianceSh9{}; // L2 SH diffuse sky irradiance (ambient floor)
        glm::mat4 prevViewProjection{1.0f};
        // Previous frame's VIEW matrix alone: the glass virtual-motion replay composes it with the
        // CURRENT jittered projection so the replayed prev ray shares the current sub-pixel offset
        // (jitter cancels out of the virtual MV; static glass MV stays exactly 0). Composing with
        // the prev UNJITTERED projection after the jittered-primaries change (cab2529) made the
        // replayed refraction path differ from the current one by a per-frame jitter delta, which
        // refraction amplifies into frame-varying MVs on static glass -> RR boiled static glass.
        glm::mat4 prevView{1.0f};
        glm::vec3 prevCameraPos{0.0f};
        bool motionHistoryValid = false;
        // Real-time (DLSS): pixel-center primary rays through the jittered projection. Reference:
        // shader-side sub-pixel jitter for accumulation.
        bool centerPrimaryRays = false;
        std::uintptr_t envEquirectSrvCpuHandle = 0;
        std::uint32_t envImportanceCdfSrvIndex = UINT32_MAX;
        std::uint32_t envImportanceSampleCount = 0;
        std::uint32_t envImportanceCdfWidth = 0;
        std::uint32_t envImportanceCdfHeight = 0;
        float envImportanceWeightSum = 0.0f;
        float envDirectLightingLuminanceClamp = 0.0f;
        // ReSTIR DI initial sampling (roadmap P2): per-category candidate count, 0 = off (plain NEE).
        std::uint32_t restirDiCandidateCount = 0;
        bool restirGiInitialEnabled = false;
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
        float maxTraceDistance,
        int ptMaxBounces,
        bool ptRussianRoulette,
        bool ptFireflyClamp,
        float ptAmbientStrength,
        int ptAmbientAoRayCount,
        int ptDebugIsolateMode = 0);

    void Release();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const { return DxrDispatchBase::IsPipelineReady(); }
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }

    std::uintptr_t GetPrimaryOutputSrvCpuHandle() const;
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle() const;
    ID3D12Resource* GetPrimaryOutputResource() const;
    std::uint32_t GetPrimaryOutputResourceState() const;
    void SetPrimaryOutputResourceState(std::uint32_t state);
    ID3D12Resource* GetPathTracerDepthResource() const;
    std::uint32_t GetPathTracerDepthResourceState() const;
    std::uintptr_t GetPathTracerDepthSrvCpuHandle() const;
    std::uintptr_t GetPathTracerMotionSrvCpuHandle() const;
    ID3D12Resource* GetPathTracerMotionResource() const;
    std::uint32_t GetPathTracerMotionResourceState() const;
    // P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md).
    std::uintptr_t GetPathTracerDiffuseAlbedoSrvCpuHandle() const;
    std::uintptr_t GetPathTracerSpecularAlbedoSrvCpuHandle() const;
    std::uintptr_t GetPathTracerNormalRoughnessSrvCpuHandle() const;
    bool IsPathTracerPrevSurfaceHistoryValid() const;
    std::uintptr_t GetPathTracerPrevDepthSrvCpuHandle() const;
    std::uintptr_t GetPathTracerPrevNormalRoughnessSrvCpuHandle() const;
    bool HasRestirBuffers() const;
    bool HasValidOutput() const;

    // R2 temporal reuse (real-time only). Call after DispatchIfEnabled, before surface-history copy.
    // shadeOutput=false keeps isolate AOVs (reservoirs still update).
    bool DispatchRestirTemporal(
        DxrRestirDispatch& restirDispatch,
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        void* commandList,
        float maxTraceDistance,
        std::uint32_t sceneVersion,
        std::uint32_t motionVersion,
        bool realTimeMode,
        bool shadeOutput = true);

    // R3 spatial reuse (real-time only). Call after temporal, before surface-history copy.
    bool DispatchRestirSpatial(
        DxrRestirDispatch& restirDispatch,
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        void* commandList,
        float maxTraceDistance,
        bool realTimeMode,
        bool shadeOutput = true);

    void FinalizePathTracerSurfaceHistory(void* commandList);
    void InvalidateRestirHistory() { m_dispatchContext.InvalidateRestirHistory(); }

    // Resets per-pixel RNG salt so material edits (IOR, transmission, etc.) converge immediately.
    void ResetAccumulation() { m_frameIndex = 0; }

private:
    bool EnsurePipeline(std::string& outError);

    bool m_dispatchedThisFrame = false;
    std::uint32_t m_frameIndex = 0;
    std::uintptr_t m_lastEnvEquirectSrvCpuHandle = 0;
    std::uint32_t m_lastEnvImportanceCdfSrvIndex = UINT32_MAX;
    std::uint32_t m_lastEnvImportanceCount = 0;
    std::uint32_t m_lastEnvCdfWidth = 0;
    std::uint32_t m_lastEnvCdfHeight = 0;
    float m_lastEnvironmentIntensity = 1.0f;
    float m_lastEnvDirectLuminanceClamp = 0.0f;
    float m_lastSunIntensity = 0.0f;
    glm::vec3 m_lastSunDirection{0.0f, 1.0f, 0.0f};
    float m_lastSunAngularTanRadius = 0.0f;
    std::uint32_t m_lastDebugMode = 0;
    glm::vec3 m_lastPrevCameraPos{0.0f};
};
