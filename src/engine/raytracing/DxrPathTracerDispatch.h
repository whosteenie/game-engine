#pragma once

#include "engine/raytracing/DxrDispatchBase.h"
#include "engine/rendering/TemporalCameraPacket.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <glm/glm.hpp>

class Camera;
class DxrAccelerationStructures;
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

    // GPU-independent lifecycle used by each viewport state. Identity advances only after a
    // successful render, so skipped/failed evaluations cannot manufacture compatible history.
    class ViewportSequenceState
    {
    public:
        explicit ViewportSequenceState(const std::uint32_t viewportId) : m_viewportId(viewportId) {}

        ViewportSequenceState(const ViewportSequenceState&) = delete;
        ViewportSequenceState& operator=(const ViewportSequenceState&) = delete;

        std::uint32_t GetViewportId() const { return m_viewportId; }
        std::uint32_t GetAccumulationFrameIndex() const { return m_frameIndex; }
        bool DispatchedThisFrame() const { return m_dispatchedThisFrame; }

        // A successful PT dispatch is the only event that advances this sequence. Public so the
        // CPU ownership fixture exercises the same commit rule as production.
        void CommitRenderedFrame()
        {
            ++m_frameIndex;
            m_dispatchedThisFrame = true;
        }
        void BeginEvaluation() { m_dispatchedThisFrame = false; }
        void ResetAccumulation() { m_frameIndex = 0; }

    private:
        const std::uint32_t m_viewportId;
        bool m_dispatchedThisFrame = false;
        std::uint32_t m_frameIndex = 0;
    };

    // Mutable PT state for one stable viewport sequence. Pipeline objects remain on the parent
    // DxrPathTracerDispatch and are shared; every resource, camera packet, reservoir-valid bit,
    // and accumulation counter that can encode a rendered frame lives here instead.
    class ViewportState : public ViewportSequenceState
    {
    public:
        explicit ViewportState(std::uint32_t viewportId);

        ViewportState(const ViewportState&) = delete;
        ViewportState& operator=(const ViewportState&) = delete;

    private:
        friend class DxrPathTracerDispatch;
        void ReleaseResources();
        void ResetProjectResources();

        DxrDispatchContext m_dispatchContext;
        bool m_activeDiagnosticPermutation = false;
        bool m_activeSerPermutation = false;
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
        std::uint32_t viewportId,
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
        bool ptDeterministicOpticalSplit,
        bool ptIndependentOpticalRrLayers,
        bool ptOpticalMotionReplay,
        float ptAmbientStrength,
        int ptAmbientAoRayCount,
        int ptDebugIsolateMode = 0);

    void Release();
    void ResetProjectResources();

    using PipelineWarmupProgress = std::function<void(int step, int stepCount, const char* label)>;
    bool WarmUpPipelineIfNeeded(const PipelineWarmupProgress& progress = {});
    bool IsPipelineReady() const;
    bool DispatchedThisFrame(std::uint32_t viewportId) const;
    SerOverride GetSerOverride() const { return m_serOverride; }
    void SetSerOverride(SerOverride value);
    bool IsSerActive(std::uint32_t viewportId) const;
    static bool ShouldUseSerPermutation(const bool supported, const SerOverride override)
    {
        return supported && override != SerOverride::ForceOff;
    }
    static bool IsSupportedViewportId(const std::uint32_t viewportId)
    {
        return viewportId == 0 || viewportId == 1;
    }

    std::uintptr_t GetPrimaryOutputSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle(std::uint32_t viewportId) const;
    ID3D12Resource* GetPrimaryOutputResource(std::uint32_t viewportId) const;
    std::uint32_t GetPrimaryOutputResourceState(std::uint32_t viewportId) const;
    void SetPrimaryOutputResourceState(std::uint32_t viewportId, std::uint32_t state);
    ID3D12Resource* GetPathTracerDepthResource(std::uint32_t viewportId) const;
    std::uint32_t GetPathTracerDepthResourceState(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerDepthSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerMotionSrvCpuHandle(std::uint32_t viewportId) const;
    ID3D12Resource* GetPathTracerMotionResource(std::uint32_t viewportId) const;
    std::uint32_t GetPathTracerMotionResourceState(std::uint32_t viewportId) const;
    // P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md).
    std::uintptr_t GetPathTracerDiffuseAlbedoSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerSpecularAlbedoSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerNormalRoughnessSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionOutputSrvCpuHandle(std::uint32_t viewportId) const;
    ID3D12Resource* GetPathTracerOpticalTransmissionOutputResource(std::uint32_t viewportId) const;
    std::uint32_t GetPathTracerOpticalTransmissionOutputResourceState(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionDepthSrvCpuHandle(std::uint32_t viewportId) const;
    ID3D12Resource* GetPathTracerOpticalTransmissionDepthResource(std::uint32_t viewportId) const;
    std::uint32_t GetPathTracerOpticalTransmissionDepthResourceState(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionMotionSrvCpuHandle(std::uint32_t viewportId) const;
    ID3D12Resource* GetPathTracerOpticalTransmissionMotionResource(std::uint32_t viewportId) const;
    std::uint32_t GetPathTracerOpticalTransmissionMotionResourceState(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionDiffuseAlbedoSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionSpecularAlbedoSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerOpticalTransmissionNormalRoughnessSrvCpuHandle(std::uint32_t viewportId) const;
    bool IsPathTracerPrevSurfaceHistoryValid(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerPrevDepthSrvCpuHandle(std::uint32_t viewportId) const;
    std::uintptr_t GetPathTracerPrevNormalRoughnessSrvCpuHandle(std::uint32_t viewportId) const;
    bool HasRestirBuffers(std::uint32_t viewportId) const;
    bool HasValidOutput(std::uint32_t viewportId) const;

    // R2 temporal reuse (real-time only). Call after DispatchIfEnabled, before surface-history copy.
    // shadeOutput=false keeps isolate AOVs (reservoirs still update).
    bool DispatchRestirTemporal(
        std::uint32_t viewportId,
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
        std::uint32_t viewportId,
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

    void FinalizePathTracerSurfaceHistory(std::uint32_t viewportId, void* commandList);
    void InvalidateRestirHistory(std::uint32_t viewportId);

    // Resets per-pixel RNG salt so material edits (IOR, transmission, etc.) converge immediately.
    void ResetAccumulation(std::uint32_t viewportId);
    std::uint32_t GetAccumulationFrameIndex(std::uint32_t viewportId) const;

private:
    bool EnsurePipeline(bool diagnosticPermutation, bool serPermutation, std::string& outError);
    ViewportState& StateFor(std::uint32_t viewportId);
    const ViewportState& StateFor(std::uint32_t viewportId) const;

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
    SerOverride m_serOverride = SerOverride::Automatic;
    ViewportState m_sceneViewportState{0};
    ViewportState m_gameViewportState{1};
};
