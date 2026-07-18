#pragma once

#include "engine/rendering/core/HistoryCompatibility.h"

#include <cstdint>

struct ReconstructionJitterIdentity
{
    HistoryReconstructionFeature feature = HistoryReconstructionFeature::None;
    HistoryReconstructionQuality quality = HistoryReconstructionQuality::None;
    std::uint8_t qualityVersion = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
};

struct ReconstructionJitterSample
{
    std::uint32_t period = 1;
    std::uint32_t phase = 0;
    float xNdc = 0.0f;
    float yNdc = 0.0f;
    bool previousValid = false;
    std::uint32_t previousPhase = 0;
    float previousXNdc = 0.0f;
    float previousYNdc = 0.0f;
    bool startsNewHistory = false;
};

bool ReconstructionUsesJitter(HistoryReconstructionFeature feature);
std::uint32_t ReconstructionJitterPeriod(
    HistoryReconstructionFeature feature,
    HistoryReconstructionQuality quality);
bool ReconstructionJitterNeedsPhaseZero(
    const HistoryCompatibilityState& history,
    const ReconstructionJitterIdentity& identity,
    bool cameraPacketValid,
    bool cameraCut);

// Pure per-viewport phase owner. Prepare is non-committing because a viewport may be skipped after
// camera setup. The phase advances only after S1 commits a rendered compatible identity.
class ReconstructionJitterState
{
public:
    const ReconstructionJitterSample& Prepare(
        const ReconstructionJitterIdentity& identity,
        bool startsNewHistory);
    // Called only from the S1 reset application path. Returns false if S1 requested a jitter reset
    // after a nonzero phase had already been selected for the current frame.
    bool ResetThroughHistoryCompatibility();
    bool CommitRendered();
    void CancelPrepared();
    void ResetImmediate();

    bool HasPreparedSample() const { return m_hasPrepared; }
    const ReconstructionJitterSample& PreparedSample() const { return m_prepared; }
    std::uint32_t NextPhase() const { return m_nextPhase; }

private:
    ReconstructionJitterSample m_prepared{};
    ReconstructionJitterSample m_previous{};
    std::uint32_t m_nextPhase = 0;
    bool m_hasPrepared = false;
    bool m_previousValid = false;
};
