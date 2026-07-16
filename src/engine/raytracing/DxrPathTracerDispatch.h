#pragma once

#include "engine/raytracing/DxrDispatchBase.h"
#include "engine/rendering/TemporalCameraPacket.h"

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
    enum class SerOverride : std::uint8_t
    {
        Automatic,
        ForceOff,
        ForceOn,
    };

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
        float environmentRotationYRadians = 0.0f;
        float maxReflectionLod = 4.0f;
        glm::vec3 sunDirection = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor = glm::vec3(1.0f);
        float sunIntensity = 0.0f;
        float sunAngularRadiusDegrees = 0.27f;
        std::array<glm::vec4, 9> irradianceSh9{}; // L2 SH diffuse sky irradiance (ambient floor)
        // One complete current/previous packet, populated from the active viewport's
        // ScreenSpaceEffects history. No application-global or identity/zero history fallback.
        TemporalCameraPacket cameraPacket{};
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

    // CPU-side camera portion of the shader constants. The packet stores an unjittered projection
    // plus explicit NDC jitter. For real-time pixel-center primaries, viewProjection and
    // inverseViewProjection use the current jitter; unjitteredViewProjection and
    // previousViewProjection never do. previousReplayInverseViewProjection intentionally combines
    // the previous view with the CURRENT effective projection so static-glass jitter cancels.
    struct CameraConstants
    {
        glm::mat4 view{0.0f};
        glm::mat4 viewProjection{0.0f};
        glm::mat4 inverseViewProjection{0.0f};
        glm::mat4 unjitteredViewProjection{0.0f};
        glm::mat4 previousViewProjection{0.0f};
        glm::mat4 previousReplayInverseViewProjection{0.0f};
        glm::vec3 worldPosition{0.0f};
        glm::vec3 previousWorldPosition{0.0f};
        bool historyValid = false;
    };

    // Returns false only when the current state is incomplete. An incomplete previous state
    // produces a valid current-frame packing with historyValid=false and current-state fallback
    // values that temporal shaders are forbidden to consume.
    static bool TryBuildCameraConstants(
        const TemporalCameraPacket& packet,
        bool centerPrimaryRays,
        CameraConstants& outConstants)
    {
        outConstants = {};
        if (!TemporalCamera::IsComplete(packet.current))
        {
            return false;
        }

        const bool historyValid = TemporalCamera::IsComplete(packet.previous);
        const TemporalCameraState& previous = historyValid ? packet.previous : packet.current;
        const glm::mat4 effectiveProjection = centerPrimaryRays
            ? TemporalCamera::ApplyJitter(packet.current.projection, packet.current.jitterNdc)
            : packet.current.projection;

        outConstants.view = packet.current.view;
        outConstants.viewProjection = effectiveProjection * packet.current.view;
        outConstants.inverseViewProjection = centerPrimaryRays
            ? glm::inverse(outConstants.viewProjection)
            : packet.current.inverseViewProjection;
        outConstants.unjitteredViewProjection =
            packet.current.projection * packet.current.view;
        outConstants.previousViewProjection = previous.projection * previous.view;
        outConstants.previousReplayInverseViewProjection =
            glm::inverse(effectiveProjection * previous.view);
        outConstants.worldPosition = packet.current.worldPosition;
        outConstants.previousWorldPosition = previous.worldPosition;
        outConstants.historyValid = historyValid;
        return true;
    }

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
    void ResetProjectResources();

    bool WarmUpPipelineIfNeeded();
    bool IsPipelineReady() const;
    bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }
    SerOverride GetSerOverride() const { return m_serOverride; }
    void SetSerOverride(SerOverride value);
    bool IsSerActive() const { return m_activeSerPermutation; }
    static bool ShouldUseSerPermutation(const bool supported, const SerOverride override)
    {
        return supported && override != SerOverride::ForceOff;
    }

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
        bool enableDiTemporal,
        bool enableGiTemporal,
        bool shadeOutput = true);

    // R3 spatial reuse (real-time only). Call after temporal, before surface-history copy.
    bool DispatchRestirSpatial(
        DxrRestirDispatch& restirDispatch,
        const DxrAccelerationStructures& accelerationStructures,
        const Camera& camera,
        void* commandList,
        float maxTraceDistance,
        bool realTimeMode,
        bool enableDiSpatial,
        bool enableGiBoilingFilter,
        bool enableGiSpatialReuse,
        bool shadeOutput = true);

    void FinalizePathTracerSurfaceHistory(void* commandList);
    void InvalidateRestirHistory() { m_dispatchContext.InvalidateRestirHistory(); }

    // Resets per-pixel RNG salt so material edits (IOR, transmission, etc.) converge immediately.
    void ResetAccumulation() { m_frameIndex = 0; }

private:
    bool EnsurePipeline(bool diagnosticPermutation, bool serPermutation, std::string& outError);

    bool m_dispatchedThisFrame = false;
    // The production pipeline lives in DxrDispatchBase. Diagnostics use an identical root/SBT
    // layout but a separate compile-time shader permutation, kept warm to make mode changes cheap.
    DxrPipeline m_diagnosticPipeline;
    ShaderBindingTable m_diagnosticShaderBindingTable;
    bool m_diagnosticPipelineReady = false;
    DxrPipeline m_serPipeline;
    ShaderBindingTable m_serShaderBindingTable;
    bool m_serPipelineReady = false;
    DxrPipeline m_serDiagnosticPipeline;
    ShaderBindingTable m_serDiagnosticShaderBindingTable;
    bool m_serDiagnosticPipelineReady = false;
    bool m_activeDiagnosticPermutation = false;
    bool m_activeSerPermutation = false;
    SerOverride m_serOverride = SerOverride::Automatic;
    std::uint32_t m_frameIndex = 0;
    std::uintptr_t m_lastEnvEquirectSrvCpuHandle = 0;
    std::uint32_t m_lastEnvImportanceCdfSrvIndex = UINT32_MAX;
    std::uint32_t m_lastEnvImportanceCount = 0;
    std::uint32_t m_lastEnvCdfWidth = 0;
    std::uint32_t m_lastEnvCdfHeight = 0;
    float m_lastEnvironmentIntensity = 1.0f;
    float m_lastEnvironmentRotationYRadians = 0.0f;
    float m_lastEnvDirectLuminanceClamp = 0.0f;
    float m_lastSunIntensity = 0.0f;
    glm::vec3 m_lastSunDirection{0.0f, 1.0f, 0.0f};
    float m_lastSunAngularTanRadius = 0.0f;
    std::uint32_t m_lastDebugMode = 0;
    TemporalCameraPacket m_lastCameraPacket{};
    CameraConstants m_lastCameraConstants{};
};
