#include "engine/rendering/HistoryCompatibility.h"

namespace
{
    bool UsesDisplayReconstruction(const HistoryReconstructionFeature feature)
    {
        return feature == HistoryReconstructionFeature::Dlss
            || feature == HistoryReconstructionFeature::RayReconstruction;
    }

    std::uint32_t OwnersForReasons(
        const std::uint32_t reasons,
        const HistoryCompatibilityKey* previous,
        const HistoryCompatibilityKey& current)
    {
        using namespace HistoryCompatibilityReason;
        using namespace HistoryCompatibilityOwner;

        std::uint32_t owners = 0;
        if ((reasons & (FirstFrame | Producer | RenderExtent | CameraInvalid | CameraCut)) != 0)
        {
            owners |= All;
        }
        if ((reasons & (Guide | Feature | Quality | OutputExtent | DiagnosticSignal
                | OpticalDomain)) != 0)
        {
            owners |= Reconstruction | DisplayBloom;
        }
        if ((reasons & Feature) != 0 && previous != nullptr
            && (!UsesDisplayReconstruction(previous->feature)
                || !UsesDisplayReconstruction(current.feature)))
        {
            // Crossing the render-res/display-res reconstruction boundary changes which bloom
            // history is active. DLSS<->RR stays wholly in display-res bloom and does not do this.
            owners |= RenderBloom;
        }
        return owners;
    }
}

bool HistoryCompatibilityKey::operator==(const HistoryCompatibilityKey& rhs) const
{
    return producer == rhs.producer
        && guideProducer == rhs.guideProducer
        && guideVersion == rhs.guideVersion
        && feature == rhs.feature
        && quality == rhs.quality
        && qualityVersion == rhs.qualityVersion
        && renderWidth == rhs.renderWidth
        && renderHeight == rhs.renderHeight
        && outputWidth == rhs.outputWidth
        && outputHeight == rhs.outputHeight
        && cameraPacketValid == rhs.cameraPacketValid
        && cameraCut == rhs.cameraCut
        && opticalSceneVersion == rhs.opticalSceneVersion
        && opticalMotionVersion == rhs.opticalMotionVersion
        && diagnosticSignal == rhs.diagnosticSignal;
}

HistoryCompatibilityTransition HistoryCompatibilityState::Begin(
    const HistoryCompatibilityKey& key)
{
    if (m_hasPending && key == m_pending)
    {
        HistoryCompatibilityTransition duplicate = m_pendingTransition;
        duplicate.ownerBits = 0;
        duplicate.scheduled = false;
        return duplicate;
    }

    std::uint32_t reasons = 0;
    if (!m_hasCommitted)
    {
        reasons |= HistoryCompatibilityReason::FirstFrame;
    }
    else
    {
        if (key.producer != m_committed.producer)
        {
            reasons |= HistoryCompatibilityReason::Producer;
        }
        if (key.guideProducer != m_committed.guideProducer
            || key.guideVersion != m_committed.guideVersion)
        {
            reasons |= HistoryCompatibilityReason::Guide;
        }
        if (key.feature != m_committed.feature)
        {
            reasons |= HistoryCompatibilityReason::Feature;
        }
        if (key.quality != m_committed.quality
            || key.qualityVersion != m_committed.qualityVersion)
        {
            reasons |= HistoryCompatibilityReason::Quality;
        }
        if (key.renderWidth != m_committed.renderWidth
            || key.renderHeight != m_committed.renderHeight)
        {
            reasons |= HistoryCompatibilityReason::RenderExtent;
        }
        if (key.outputWidth != m_committed.outputWidth
            || key.outputHeight != m_committed.outputHeight)
        {
            reasons |= HistoryCompatibilityReason::OutputExtent;
        }
        if (key.diagnosticSignal != m_committed.diagnosticSignal)
        {
            reasons |= HistoryCompatibilityReason::DiagnosticSignal;
        }
        // Object motion is handled by the PT motion guide and per-pixel surface validation.
        // Only a structural/material optical-domain change invalidates the viewport history.
        if (key.opticalSceneVersion != m_committed.opticalSceneVersion)
        {
            reasons |= HistoryCompatibilityReason::OpticalDomain;
        }
    }

    // Camera validity/cut are frame-boundary events. A rendered reset frame establishes fresh
    // history, so returning from cut/invalid to valid must not manufacture a second reset.
    if (!key.cameraPacketValid)
    {
        reasons |= HistoryCompatibilityReason::CameraInvalid;
    }
    if (key.cameraCut)
    {
        reasons |= HistoryCompatibilityReason::CameraCut;
    }

    m_pending = key;
    m_pendingTransition.reasonBits = reasons;
    m_pendingTransition.ownerBits = OwnersForReasons(
        reasons, m_hasCommitted ? &m_committed : nullptr, key);
    m_pendingTransition.scheduled = reasons != 0;
    m_hasPending = true;
    return m_pendingTransition;
}

bool HistoryCompatibilityState::CommitRendered()
{
    if (!m_hasPending)
    {
        return false;
    }

    m_committed = m_pending;
    m_committed.cameraPacketValid = true;
    m_committed.cameraCut = false;
    m_hasCommitted = true;
    m_hasPending = false;
    m_pendingTransition = {};
    return true;
}

void HistoryCompatibilityState::CancelPending()
{
    m_hasPending = false;
    m_pendingTransition = {};
}

const char* HistoryRenderProducerName(const HistoryRenderProducer producer)
{
    switch (producer)
    {
    case HistoryRenderProducer::Raster: return "raster";
    case HistoryRenderProducer::Hybrid: return "hybrid";
    case HistoryRenderProducer::PathTracer: return "path-tracer";
    }
    return "unknown";
}

const char* HistoryGuideProducerName(const HistoryGuideProducer producer)
{
    switch (producer)
    {
    case HistoryGuideProducer::Raster: return "raster-guides-v1";
    case HistoryGuideProducer::PathTracer: return "pt-guides-v1";
    case HistoryGuideProducer::Mixed: return "mixed-guides-v1";
    }
    return "unknown";
}

const char* HistoryReconstructionFeatureName(const HistoryReconstructionFeature feature)
{
    switch (feature)
    {
    case HistoryReconstructionFeature::None: return "none";
    case HistoryReconstructionFeature::Taa: return "taa";
    case HistoryReconstructionFeature::Dlss: return "dlss";
    case HistoryReconstructionFeature::RayReconstruction: return "rr";
    }
    return "unknown";
}

const char* HistoryReconstructionQualityName(const HistoryReconstructionQuality quality)
{
    switch (quality)
    {
    case HistoryReconstructionQuality::None: return "none";
    case HistoryReconstructionQuality::Taa: return "taa";
    case HistoryReconstructionQuality::Dlaa: return "dlaa";
    case HistoryReconstructionQuality::Quality: return "quality";
    case HistoryReconstructionQuality::Balanced: return "balanced";
    case HistoryReconstructionQuality::Performance: return "performance";
    case HistoryReconstructionQuality::UltraPerformance: return "ultra-performance";
    }
    return "unknown";
}
