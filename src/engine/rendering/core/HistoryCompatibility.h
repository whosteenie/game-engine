#pragma once

#include <cstdint>

enum class HistoryRenderProducer : std::uint8_t
{
    Raster,
    Hybrid,
    PathTracer,
};

enum class HistoryGuideProducer : std::uint8_t
{
    Raster,
    PathTracer,
    Mixed,
};

enum class HistoryReconstructionFeature : std::uint8_t
{
    None,
    Taa,
    Dlss,
    RayReconstruction,
};

enum class HistoryReconstructionQuality : std::uint8_t
{
    None,
    Taa,
    Dlaa,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

struct HistoryCompatibilityKey
{
    HistoryRenderProducer producer = HistoryRenderProducer::Raster;
    HistoryGuideProducer guideProducer = HistoryGuideProducer::Raster;
    std::uint8_t guideVersion = 1;
    HistoryReconstructionFeature feature = HistoryReconstructionFeature::None;
    HistoryReconstructionQuality quality = HistoryReconstructionQuality::None;
    // Feature-specific model identity (currently the RR preset).
    std::uint8_t qualityVersion = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    bool cameraPacketValid = false;
    bool cameraCut = false;
    // Path-traced optical-domain generations. Structural/material changes invalidate the viewport;
    // transform motion is consumed by per-pixel motion/surface validation instead.
    std::uint32_t opticalSceneVersion = 0;
    std::uint32_t opticalMotionVersion = 0;
    // Only diagnostics that alter the rendered/reconstruction signal are represented.
    std::uint32_t diagnosticSignal = 0;

    bool operator==(const HistoryCompatibilityKey& rhs) const;
    bool operator!=(const HistoryCompatibilityKey& rhs) const { return !(*this == rhs); }
};

namespace HistoryCompatibilityReason
{
    constexpr std::uint32_t FirstFrame = 1u << 0;
    constexpr std::uint32_t Producer = 1u << 1;
    constexpr std::uint32_t Guide = 1u << 2;
    constexpr std::uint32_t Feature = 1u << 3;
    constexpr std::uint32_t Quality = 1u << 4;
    constexpr std::uint32_t RenderExtent = 1u << 5;
    constexpr std::uint32_t OutputExtent = 1u << 6;
    constexpr std::uint32_t CameraInvalid = 1u << 7;
    constexpr std::uint32_t CameraCut = 1u << 8;
    constexpr std::uint32_t DiagnosticSignal = 1u << 9;
    constexpr std::uint32_t OpticalDomain = 1u << 10;
}

namespace HistoryCompatibilityOwner
{
    constexpr std::uint32_t Reconstruction = 1u << 0;
    constexpr std::uint32_t RenderBloom = 1u << 1;
    constexpr std::uint32_t DisplayBloom = 1u << 2;
    constexpr std::uint32_t PtReferenceAccumulation = 1u << 3;
    constexpr std::uint32_t RestirTemporal = 1u << 4;
    constexpr std::uint32_t All = Reconstruction | RenderBloom | DisplayBloom
        | PtReferenceAccumulation | RestirTemporal;
}

struct HistoryCompatibilityTransition
{
    std::uint32_t reasonBits = 0;
    std::uint32_t ownerBits = 0;
    // True only for the first Begin call for this pending frame identity.
    bool scheduled = false;

    bool IsCompatible() const { return reasonBits == 0; }
    bool Resets(const std::uint32_t owner) const { return (ownerBits & owner) != 0; }
};

// Pure per-viewport state machine. The owner embeds one instance in each ScreenSpaceEffects object;
// it intentionally contains no global viewport map.
class HistoryCompatibilityState
{
public:
    HistoryCompatibilityTransition Begin(const HistoryCompatibilityKey& key);
    bool CommitRendered();
    void CancelPending();

    bool HasCommittedKey() const { return m_hasCommitted; }
    bool HasPendingKey() const { return m_hasPending; }
    const HistoryCompatibilityKey& CommittedKey() const { return m_committed; }
    const HistoryCompatibilityKey& PendingKey() const { return m_pending; }

private:
    HistoryCompatibilityKey m_committed{};
    HistoryCompatibilityKey m_pending{};
    HistoryCompatibilityTransition m_pendingTransition{};
    bool m_hasCommitted = false;
    bool m_hasPending = false;
};

const char* HistoryRenderProducerName(HistoryRenderProducer producer);
const char* HistoryGuideProducerName(HistoryGuideProducer producer);
const char* HistoryReconstructionFeatureName(HistoryReconstructionFeature feature);
const char* HistoryReconstructionQualityName(HistoryReconstructionQuality quality);
