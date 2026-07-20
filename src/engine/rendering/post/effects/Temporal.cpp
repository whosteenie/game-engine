#include "engine/rendering/post/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rhi/DlssContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace
{
    DlssQuality ToDlssQuality(const DlssPreset preset)
    {
        switch (preset)
        {
        case DlssPreset::Quality: return DlssQuality::Quality;
        case DlssPreset::Balanced: return DlssQuality::Balanced;
        case DlssPreset::Performance: return DlssQuality::Performance;
        case DlssPreset::UltraPerformance: return DlssQuality::UltraPerformance;
        default: return DlssQuality::Quality;
        }
    }
}

void ScreenSpaceEffects::UpdatePlannedReconstructionExtent()
{
    if (m_antiAliasingMode != AntiAliasingMode::DLAA
        && m_antiAliasingMode != AntiAliasingMode::DLSS)
    {
        m_plannedReconstructionExtent = {};
        return;
    }

    const DlssExtentRecommendationKey key = BuildActiveDlssExtentKey();
    m_plannedReconstructionExtent = DlssContext::Get().PlanReconstructionExtent(key);
}

DlssExtentRecommendationKey ScreenSpaceEffects::BuildActiveDlssExtentKey() const
{
    DlssExtentRecommendationKey key{};
    key.viewportId = m_dlssViewportId;
    key.outputExtent = {
        static_cast<std::uint32_t>(std::max(1, m_viewportWidth)),
        static_cast<std::uint32_t>(std::max(1, m_viewportHeight))};
    key.feature = IsRayReconstructionActive()
        ? DlssReconstructionFeature::RayReconstruction
        : DlssReconstructionFeature::SuperResolution;
    key.quality = m_antiAliasingMode == AntiAliasingMode::DLAA
        ? DlssQuality::DLAA
        : ToDlssQuality(m_dlssPreset);
    return key;
}

float ScreenSpaceEffects::GetActiveRenderScale() const
{
    switch (m_antiAliasingMode)
    {
    case AntiAliasingMode::SSAA:
        return std::clamp(m_renderScale, 1.0f, 2.0f);
    case AntiAliasingMode::DLAA:
        // DLSS at native resolution: internal == display.
        return 1.0f;
    case AntiAliasingMode::DLSS:
        // S2-P4 uses the exact planned dimensions in GetRenderWidth/Height, not a scalar ratio.
        return 1.0f;
    default:
        return 1.0f;
    }
}

int ScreenSpaceEffects::GetRenderWidth() const
{
    if (m_antiAliasingMode == AntiAliasingMode::DLAA
        || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        const DlssExtentRecommendationKey activeKey = BuildActiveDlssExtentKey();
        if (!m_plannedReconstructionExtent.IsValid()
            || !(m_plannedReconstructionExtent.key == activeKey))
        {
            const_cast<ScreenSpaceEffects*>(this)->UpdatePlannedReconstructionExtent();
        }
        std::string reason;
        const DlssExtent active = ResolveDlssActiveRenderExtent(
            m_plannedReconstructionExtent, activeKey, reason);
        if (active.width != 0)
        {
            return static_cast<int>(active.width);
        }
        throw std::runtime_error("DLSS active extent contract rejected: " + reason);
    }
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportWidth) * GetActiveRenderScale())));
}

int ScreenSpaceEffects::GetRenderHeight() const
{
    if (m_antiAliasingMode == AntiAliasingMode::DLAA
        || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        const DlssExtentRecommendationKey activeKey = BuildActiveDlssExtentKey();
        if (!m_plannedReconstructionExtent.IsValid()
            || !(m_plannedReconstructionExtent.key == activeKey))
        {
            const_cast<ScreenSpaceEffects*>(this)->UpdatePlannedReconstructionExtent();
        }
        std::string reason;
        const DlssExtent active = ResolveDlssActiveRenderExtent(
            m_plannedReconstructionExtent, activeKey, reason);
        if (active.height != 0)
        {
            return static_cast<int>(active.height);
        }
        throw std::runtime_error("DLSS active extent contract rejected: " + reason);
    }
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportHeight) * GetActiveRenderScale())));
}

float ScreenSpaceEffects::GetAutoMaterialMipBias() const
{
    if (m_antiAliasingMode != AntiAliasingMode::DLAA && m_antiAliasingMode != AntiAliasingMode::DLSS)
    {
        return 0.0f;
    }
    if (m_viewportWidth <= 0)
    {
        return 0.0f;
    }

    const float renderScale = static_cast<float>(GetRenderWidth())
        / static_cast<float>(m_viewportWidth);
    return std::log2(renderScale);
}

ReconstructionJitterIdentity ScreenSpaceEffects::BuildReconstructionJitterIdentity(
    const int outputWidth,
    const int outputHeight) const
{
    ReconstructionJitterIdentity identity{};

    if (m_antiAliasingMode == AntiAliasingMode::TAA)
    {
        identity.feature = HistoryReconstructionFeature::Taa;
        identity.quality = HistoryReconstructionQuality::Taa;
    }
    else if (m_antiAliasingMode == AntiAliasingMode::DLAA
        || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        identity.feature = IsRayReconstructionActive()
            ? HistoryReconstructionFeature::RayReconstruction
            : HistoryReconstructionFeature::Dlss;
        if (m_antiAliasingMode == AntiAliasingMode::DLAA)
        {
            identity.quality = HistoryReconstructionQuality::Dlaa;
        }
        else
        {
            switch (m_dlssPreset)
            {
            case DlssPreset::Quality:
                identity.quality = HistoryReconstructionQuality::Quality;
                break;
            case DlssPreset::Balanced:
                identity.quality = HistoryReconstructionQuality::Balanced;
                break;
            case DlssPreset::Performance:
                identity.quality = HistoryReconstructionQuality::Performance;
                break;
            case DlssPreset::UltraPerformance:
                identity.quality = HistoryReconstructionQuality::UltraPerformance;
                break;
            }
        }
        identity.qualityVersion =
            identity.feature == HistoryReconstructionFeature::RayReconstruction
            ? static_cast<std::uint8_t>(m_rrPreset)
            : 0;
    }

    identity.renderWidth = m_width;
    identity.renderHeight = m_height;
    identity.outputWidth = outputWidth;
    identity.outputHeight = outputHeight;
    return identity;
}

HistoryCompatibilityKey ScreenSpaceEffects::BuildHistoryCompatibilityKey(
    const HistoryRenderProducer producer,
    const Camera& camera,
    const int outputWidth,
    const int outputHeight,
    const std::uint32_t opticalSceneVersion,
    const std::uint32_t opticalMotionVersion) const
{
    HistoryCompatibilityKey key{};
    key.producer = producer;
    const ReconstructionJitterIdentity jitterIdentity =
        BuildReconstructionJitterIdentity(outputWidth, outputHeight);
    key.feature = jitterIdentity.feature;
    key.quality = jitterIdentity.quality;
    key.qualityVersion = jitterIdentity.qualityVersion;

    const bool reconstructionUsesGuides =
        key.feature == HistoryReconstructionFeature::Dlss
        || key.feature == HistoryReconstructionFeature::RayReconstruction;
    if (reconstructionUsesGuides && producer == HistoryRenderProducer::PathTracer)
    {
        if (m_ptRrBundleMode == 0)
        {
            key.guideProducer = HistoryGuideProducer::PathTracer;
        }
        else if (m_ptRrBundleMode > 1)
        {
            key.guideProducer = HistoryGuideProducer::Mixed;
        }
    }

    key.renderWidth = jitterIdentity.renderWidth;
    key.renderHeight = jitterIdentity.renderHeight;
    key.outputWidth = jitterIdentity.outputWidth;
    key.outputHeight = jitterIdentity.outputHeight;
    const TemporalCameraState currentCamera = TemporalCamera::MakeState(
        camera.GetViewMatrix(),
        camera.GetUnjitteredProjectionMatrix(),
        glm::inverse(camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix()),
        camera.GetPosition(),
        camera.GetProjectionJitter());
    key.cameraPacketValid = TemporalCamera::IsComplete(currentCamera)
        && m_motionVectorFrameState.historyValid
        && TemporalCamera::IsComplete(m_motionVectorFrameState.previousCamera);
    key.cameraCut = key.cameraPacketValid
        && DlssResolvePass::DetectCameraCut(camera.GetViewMatrix(), m_motionVectorFrameState);
    if (reconstructionUsesGuides)
    {
        key.diagnosticSignal = (m_useDilatedDlssMotionVectors ? (1u << 8) : 0u)
            | (m_reconstructDlssCameraMotion ? (1u << 9) : 0u)
            | (m_ptRrTemporalValidity ? (1u << 10) : 0u);
        if (producer == HistoryRenderProducer::PathTracer)
        {
            key.diagnosticSignal |= static_cast<std::uint32_t>(m_ptRrBundleMode & 0x7);
        }
    }
    if (producer == HistoryRenderProducer::PathTracer)
    {
        key.opticalSceneVersion = opticalSceneVersion;
        key.opticalMotionVersion = opticalMotionVersion;
        key.diagnosticSignal |=
            static_cast<std::uint32_t>(PtDebugIsolateModeFromRenderDebug(m_debugMode)) << 16;
    }
    return key;
}

HistoryCompatibilityTransition ScreenSpaceEffects::BeginHistoryCompatibilityFrame(
    const HistoryRenderProducer producer,
    const Camera& camera,
    const int outputWidth,
    const int outputHeight,
    const std::uint32_t opticalSceneVersion,
    const std::uint32_t opticalMotionVersion) const
{
    m_historyCompatibilityRenderedThisFrame = false;
    const HistoryCompatibilityKey key = BuildHistoryCompatibilityKey(
        producer, camera, outputWidth, outputHeight, opticalSceneVersion, opticalMotionVersion);
    const HistoryCompatibilityTransition transition = m_historyCompatibilityState.Begin(key);
    FrameDiagnostics::LogHistoryCompatibility(
        m_dlssViewportId,
        transition.scheduled ? "schedule" : "compatible",
        HistoryRenderProducerName(key.producer),
        HistoryGuideProducerName(key.guideProducer),
        key.guideVersion,
        HistoryReconstructionFeatureName(key.feature),
        HistoryReconstructionQualityName(key.quality),
        key.qualityVersion,
        key.renderWidth,
        key.renderHeight,
        key.outputWidth,
        key.outputHeight,
        key.cameraPacketValid,
        key.cameraCut,
        key.opticalSceneVersion,
        key.opticalMotionVersion,
        key.diagnosticSignal,
        transition.reasonBits,
        transition.ownerBits);
    if (transition.scheduled)
    {
        ApplyHistoryCompatibilityReset(key, transition);
    }
    return transition;
}

void ScreenSpaceEffects::ApplyHistoryCompatibilityReset(
    const HistoryCompatibilityKey&,
    const HistoryCompatibilityTransition& transition) const
{
    using namespace HistoryCompatibilityOwner;
    if (transition.Resets(Reconstruction))
    {
        m_taaHistoryValid = false;
        m_dlssHistoryValid = false;
        m_dlssOpticalTransmissionHistoryValid = false;
        m_rrTemporalPrimaryHistoryValid = false;
        m_rrTemporalTransmissionHistoryValid = false;
    }
    constexpr std::uint32_t kResetsJitterPhase =
        HistoryCompatibilityReason::FirstFrame
        | HistoryCompatibilityReason::Feature
        | HistoryCompatibilityReason::Quality
        | HistoryCompatibilityReason::RenderExtent
        | HistoryCompatibilityReason::OutputExtent
        | HistoryCompatibilityReason::CameraInvalid
        | HistoryCompatibilityReason::CameraCut;
    if ((transition.reasonBits & kResetsJitterPhase) != 0
        && !m_reconstructionJitterState.ResetThroughHistoryCompatibility())
    {
        throw std::logic_error(
            "S2-P3 jitter phase ownership conflicted with the S1 compatibility reset boundary.");
    }
    if (transition.Resets(RenderBloom))
    {
        m_bloomHistoryValid = false;
        m_bloomTemporalWarmupFrames = 0;
        m_prevFrameBloomSrv = 0;
    }
    if (transition.Resets(DisplayBloom))
    {
        m_dlssBloomHistoryValid = false;
        m_dlssBloomTemporalWarmupFrames = 0;
    }
    if (transition.Resets(PtReferenceAccumulation))
    {
        auto* self = const_cast<ScreenSpaceEffects*>(this);
        self->m_ptAccumSampleCount = 0;
        self->m_ptAccumHistoryKey = {};
        self->m_ptAccumPingPongReadFromScratch = false;
        self->m_ptAccumSumDisplaySrv = 0;
    }

    constexpr std::uint32_t kInvalidatesCameraHistory =
        HistoryCompatibilityReason::Producer
        | HistoryCompatibilityReason::RenderExtent
        | HistoryCompatibilityReason::OutputExtent
        | HistoryCompatibilityReason::CameraInvalid
        | HistoryCompatibilityReason::CameraCut;
    if ((transition.reasonBits & kInvalidatesCameraHistory) != 0)
    {
        m_motionVectorFrameState = {};
    }
}

bool ScreenSpaceEffects::CommitRenderedHistoryCompatibility() const
{
    if (!m_historyCompatibilityState.HasPendingKey())
    {
        return false;
    }
    const HistoryCompatibilityKey key = m_historyCompatibilityState.PendingKey();
    if (key.producer == HistoryRenderProducer::PathTracer && !m_pathTracerActive)
    {
        // The configured PT frame did not dispatch/present a PT signal (for example an empty or
        // incomplete scene). Keep comparing against the last compatible rendered identity.
        FrameDiagnostics::LogHistoryCompatibility(
            m_dlssViewportId,
            "cancel-unrendered",
            HistoryRenderProducerName(key.producer),
            HistoryGuideProducerName(key.guideProducer),
            key.guideVersion,
            HistoryReconstructionFeatureName(key.feature),
            HistoryReconstructionQualityName(key.quality),
            key.qualityVersion,
            key.renderWidth,
            key.renderHeight,
            key.outputWidth,
            key.outputHeight,
            key.cameraPacketValid,
            key.cameraCut,
            key.opticalSceneVersion,
            key.opticalMotionVersion,
            key.diagnosticSignal,
            0,
            0);
        m_historyCompatibilityState.CancelPending();
        return false;
    }
    if (!m_historyCompatibilityState.CommitRendered())
    {
        return false;
    }
    m_historyCompatibilityRenderedThisFrame = true;
    FrameDiagnostics::LogHistoryCompatibility(
        m_dlssViewportId,
        "commit",
        HistoryRenderProducerName(key.producer),
        HistoryGuideProducerName(key.guideProducer),
        key.guideVersion,
        HistoryReconstructionFeatureName(key.feature),
        HistoryReconstructionQualityName(key.quality),
        key.qualityVersion,
        key.renderWidth,
        key.renderHeight,
        key.outputWidth,
        key.outputHeight,
        key.cameraPacketValid,
        key.cameraCut,
        key.opticalSceneVersion,
        key.opticalMotionVersion,
        key.diagnosticSignal,
        0,
        0);
    return true;
}

void ScreenSpaceEffects::InvalidateTemporalHistory() const
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "reconstruction", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x10u);
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "dlss-display-bloom", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x10u);
    m_motionVectorFrameState = {};
    m_radianceHistoryValid = false;
    m_giFrameIndex = 0;
    m_giPrevViewProjection = glm::mat4(1.0f);
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
    m_prevFrameBloomSrv = 0;
    m_dlssHistoryValid = false;
    m_dlssOpticalTransmissionHistoryValid = false;
    m_rrTemporalPrimaryHistoryValid = false;
    m_rrTemporalTransmissionHistoryValid = false;
    m_dlssBloomHistoryValid = false;
    m_dlssBloomTemporalWarmupFrames = 0;
    const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
}

void ScreenSpaceEffects::ResetTaaHistory() const
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "render-bloom", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        "none", "existing-quality", m_width, m_height, m_viewportWidth, m_viewportHeight,
        false, false, 0x20u);
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "reconstruction", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x20u);
    m_taaHistoryValid = false;
    m_reconstructionJitterState.ResetImmediate();
    m_bloomHistoryValid = false;
    m_bloomTemporalWarmupFrames = 0;
    m_dlssHistoryValid = false;
    m_dlssOpticalTransmissionHistoryValid = false;
    m_rrTemporalPrimaryHistoryValid = false;
    m_rrTemporalTransmissionHistoryValid = false;
    m_dlssBloomHistoryValid = false;
    m_dlssBloomTemporalWarmupFrames = 0;
}

void ScreenSpaceEffects::Resize(const int viewportWidth, const int viewportHeight)
{
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return;
    }

    const int prevViewportWidth = m_viewportWidth;
    const int prevViewportHeight = m_viewportHeight;
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    UpdatePlannedReconstructionExtent();
    const int renderWidth = GetRenderWidth();
    const int renderHeight = GetRenderHeight();

    if (m_width == renderWidth && m_height == renderHeight && m_sceneFramebuffer->IsValid()
        && m_sceneFramebuffer->GetSampleCount() == GetEffectiveGeometryMsaaSampleCount())
    {
        const bool wantsDlssDisplay = m_antiAliasingMode == AntiAliasingMode::DLAA
            || m_antiAliasingMode == AntiAliasingMode::DLSS;
        if (wantsDlssDisplay
            && (prevViewportWidth != viewportWidth || prevViewportHeight != viewportHeight))
        {
            ResizeDlssDisplayTargets(viewportWidth, viewportHeight);
            // The output-extent key difference schedules reconstruction/display-bloom invalidation
            // once at BeginHistoryCompatibilityFrame, immediately before their next consumers.
        }
        return;
    }

    {
        SceneRenderTrace::Scope sceneFbScope("resize scene framebuffer");
        if (!m_sceneFramebuffer->Resize(
                renderWidth,
                renderHeight,
                FramebufferColorMode::SplitDirectIndirect,
                GetEffectiveGeometryMsaaSampleCount()))
        {
            throw std::runtime_error("Scene framebuffer size is invalid.");
        }

        sceneFbScope.Success();
    }
    {
        SceneRenderTrace::Scope singleChannelScope("resize single-channel targets");
        ResizeSingleChannelTargets(renderWidth, renderHeight);
        singleChannelScope.Success();
    }
    {
        SceneRenderTrace::Scope hdrScope("resize hdr targets");
        ResizeHdrColorTarget(renderWidth, renderHeight);
        hdrScope.Success();
    }
    {
        SceneRenderTrace::Scope ssrScope("resize ssr targets");
        ResizeSsrTargets(renderWidth, renderHeight);
        ssrScope.Success();
    }
    {
        SceneRenderTrace::Scope bloomScope("resize bloom targets");
        ResizeBloomTargets(renderWidth, renderHeight);
        bloomScope.Success();
    }
    {
        SceneRenderTrace::Scope ldrScope("resize ldr tonemap target");
        ResizeLdrTonemapTarget(renderWidth, renderHeight);
        ldrScope.Success();
    }
    {
        SceneRenderTrace::Scope aaTargetsScope("resize aa targets");
        ResizeAntiAliasingTargets(renderWidth, renderHeight);
        aaTargetsScope.Success();
    }
    {
        // DLSS display-res targets: HDR upscale output + post-DLSS bloom chain (S4).
        SceneRenderTrace::Scope dlssScope("resize dlss display targets");
        ResizeDlssDisplayTargets(viewportWidth, viewportHeight);
        dlssScope.Success();
    }
    m_width = renderWidth;
    m_height = renderHeight;

    // Preserve the pre-S1-P4 resize invalidation for local histories outside the audited owner
    // set. Reconstruction, both bloom domains, PT accumulation, and ReSTIR are reset once by the
    // compatibility key at the authoritative pre-consumer boundary.
    m_radianceHistoryValid = false;
    m_giFrameIndex = 0;
    m_giPrevViewProjection = glm::mat4(1.0f);
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
    ResetPathTracerTemporalDiagnostics();
}

void ScreenSpaceEffects::ReloadGeometryMsaaTargets(const int viewportWidth, const int viewportHeight)
{
    SceneRenderTrace::Scope reloadScope("ReloadGeometryMsaaTargets");
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        throw std::runtime_error("Viewport size is invalid for geometry MSAA reload.");
    }

    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    m_width = 0;
    m_height = 0;
    Resize(viewportWidth, viewportHeight);

    if (!m_sceneFramebuffer->IsValid())
    {
        throw std::runtime_error("Scene framebuffer is invalid after geometry MSAA reload.");
    }

    if (m_sceneFramebuffer->GetSampleCount() != GetEffectiveGeometryMsaaSampleCount())
    {
        throw std::runtime_error("Scene framebuffer MSAA sample count does not match the active count.");
    }

    reloadScope.Success();
}

namespace
{
    // TAA and both DLSS modes are temporal and require sub-pixel jitter on the projection.
    bool ModeUsesTemporalJitter(const AntiAliasingMode mode)
    {
        return mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::DLAA
            || mode == AntiAliasingMode::DLSS;
    }
}

void ScreenSpaceEffects::PrepareAntiAliasingFrame(Camera& camera, const bool freezeJitter) const
{
    camera.ClearProjectionJitter();
    if (!ModeUsesTemporalJitter(m_antiAliasingMode) || m_width <= 0 || m_height <= 0
        || freezeJitter)
    {
        m_reconstructionJitterState.CancelPrepared();
        return;
    }

    const ReconstructionJitterIdentity identity =
        BuildReconstructionJitterIdentity(m_viewportWidth, m_viewportHeight);
    const TemporalCameraState currentCamera = TemporalCamera::MakeState(
        camera.GetViewMatrix(),
        camera.GetUnjitteredProjectionMatrix(),
        glm::inverse(camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix()),
        camera.GetPosition(),
        glm::vec2(0.0f));
    const bool cameraPacketValid = TemporalCamera::IsComplete(currentCamera)
        && m_motionVectorFrameState.historyValid
        && TemporalCamera::IsComplete(m_motionVectorFrameState.previousCamera);
    const bool cameraCut = cameraPacketValid
        && DlssResolvePass::DetectCameraCut(camera.GetViewMatrix(), m_motionVectorFrameState);
    const bool startsNewHistory = ReconstructionJitterNeedsPhaseZero(
        m_historyCompatibilityState, identity, cameraPacketValid, cameraCut);
    const ReconstructionJitterSample& sample = m_reconstructionJitterState.Prepare(
        identity, startsNewHistory);
    camera.SetProjectionJitter(glm::vec2(sample.xNdc, sample.yNdc));
    FrameDiagnostics::LogReconstructionJitter(
        m_dlssViewportId,
        "prepare",
        HistoryReconstructionFeatureName(identity.feature),
        HistoryReconstructionQualityName(identity.quality),
        sample.period,
        sample.phase,
        sample.previousValid,
        sample.previousPhase,
        sample.startsNewHistory,
        sample.xNdc,
        sample.yNdc);
}

void ScreenSpaceEffects::FinalizeAntiAliasingFrame(const Camera& /*camera*/, const bool freezeJitter) const
{
    if (!ModeUsesTemporalJitter(m_antiAliasingMode) || freezeJitter)
    {
        m_reconstructionJitterState.CancelPrepared();
        return;
    }
    if (!m_historyCompatibilityRenderedThisFrame)
    {
        if (m_reconstructionJitterState.HasPreparedSample())
        {
            const ReconstructionJitterSample& sample =
                m_reconstructionJitterState.PreparedSample();
            FrameDiagnostics::LogReconstructionJitter(
                m_dlssViewportId,
                "cancel-unrendered",
                "unknown",
                "unknown",
                sample.period,
                sample.phase,
                sample.previousValid,
                sample.previousPhase,
                sample.startsNewHistory,
                sample.xNdc,
                sample.yNdc);
        }
        m_reconstructionJitterState.CancelPrepared();
        return;
    }
    if (m_reconstructionJitterState.HasPreparedSample())
    {
        const ReconstructionJitterIdentity identity =
            BuildReconstructionJitterIdentity(m_viewportWidth, m_viewportHeight);
        const ReconstructionJitterSample sample = m_reconstructionJitterState.PreparedSample();
        FrameDiagnostics::LogReconstructionJitter(
            m_dlssViewportId,
            "commit",
            HistoryReconstructionFeatureName(identity.feature),
            HistoryReconstructionQualityName(identity.quality),
            sample.period,
            sample.phase,
            sample.previousValid,
            sample.previousPhase,
            sample.startsNewHistory,
            sample.xNdc,
            sample.yNdc);
        m_reconstructionJitterState.CommitRendered();
    }
}

const MotionVectorFrameState& ScreenSpaceEffects::GetMotionVectorFrameState() const
{
    return m_motionVectorFrameState;
}

void ScreenSpaceEffects::AdvanceTemporalFrame(const Camera& camera) const
{
    if (!m_historyCompatibilityRenderedThisFrame)
    {
        return;
    }
    const glm::mat4 view = camera.GetViewMatrix();
    const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    const glm::mat4 unjitteredViewProjection = unjitteredProjection * view;
    m_motionVectorFrameState.previousCamera = TemporalCamera::MakeState(
        view,
        unjitteredProjection,
        glm::inverse(unjitteredViewProjection),
        camera.GetPosition(),
        camera.GetProjectionJitter());
    m_motionVectorFrameState.prevView = view;
    m_motionVectorFrameState.prevProjection = TemporalCamera::ApplyJitter(
        unjitteredProjection,
        camera.GetProjectionJitter());
    m_motionVectorFrameState.prevUnjitteredProjection = unjitteredProjection;
    m_motionVectorFrameState.prevViewProjection =
        unjitteredViewProjection;
    m_giPrevViewProjection = m_motionVectorFrameState.prevViewProjection;
    m_motionVectorFrameState.historyValid =
        TemporalCamera::IsComplete(m_motionVectorFrameState.previousCamera);
    m_historyCompatibilityRenderedThisFrame = false;
}

