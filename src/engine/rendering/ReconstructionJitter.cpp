#include "engine/rendering/ReconstructionJitter.h"

#include <algorithm>

namespace
{
    float RadicalInverse(const std::uint32_t index, const std::uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f;
        std::uint32_t i = index;
        while (i > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(i % base);
            i /= base;
        }
        return result;
    }

    ReconstructionJitterSample MakeSample(
        const ReconstructionJitterIdentity& identity,
        const std::uint32_t phase,
        const bool startsNewHistory,
        const ReconstructionJitterSample& previous,
        const bool previousValid)
    {
        ReconstructionJitterSample sample{};
        sample.period = ReconstructionJitterPeriod(identity.feature, identity.quality);
        sample.phase = phase % sample.period;
        sample.startsNewHistory = startsNewHistory;
        const float haltonX = RadicalInverse(sample.phase, 2);
        const float haltonY = RadicalInverse(sample.phase, 3);
        sample.xNdc = ((haltonX - 0.5f) * 2.0f)
            / static_cast<float>(std::max(1, identity.renderWidth));
        sample.yNdc = ((haltonY - 0.5f) * 2.0f)
            / static_cast<float>(std::max(1, identity.renderHeight));
        sample.previousValid = previousValid && !startsNewHistory;
        if (sample.previousValid)
        {
            sample.previousPhase = previous.phase;
            sample.previousXNdc = previous.xNdc;
            sample.previousYNdc = previous.yNdc;
        }
        return sample;
    }
}

bool ReconstructionUsesJitter(const HistoryReconstructionFeature feature)
{
    return feature == HistoryReconstructionFeature::Taa
        || feature == HistoryReconstructionFeature::Dlss
        || feature == HistoryReconstructionFeature::RayReconstruction;
}

std::uint32_t ReconstructionJitterPeriod(
    const HistoryReconstructionFeature feature,
    const HistoryReconstructionQuality quality)
{
    if (feature == HistoryReconstructionFeature::RayReconstruction)
    {
        // The integrated RR contract requires at least 32 phases. Preserve the existing 64-cycle
        // cadence for every exposed RR quality.
        return 64;
    }
    if (feature == HistoryReconstructionFeature::Dlss)
    {
        switch (quality)
        {
        case HistoryReconstructionQuality::Dlaa: return 8;
        case HistoryReconstructionQuality::Quality: return 18;
        case HistoryReconstructionQuality::Balanced: return 24;
        case HistoryReconstructionQuality::Performance: return 32;
        case HistoryReconstructionQuality::UltraPerformance: return 72;
        default: return 64;
        }
    }
    // TAA retains the pre-S2-P3 procedural 64-phase cadence. Non-temporal callers never prepare a
    // sample, but a unit period keeps the pure helper total for diagnostics/tests.
    return feature == HistoryReconstructionFeature::Taa ? 64 : 1;
}

bool ReconstructionJitterNeedsPhaseZero(
    const HistoryCompatibilityState& history,
    const ReconstructionJitterIdentity& identity,
    const bool cameraPacketValid,
    const bool cameraCut)
{
    if (!history.HasCommittedKey() || !cameraPacketValid || cameraCut)
    {
        return true;
    }
    const HistoryCompatibilityKey& committed = history.CommittedKey();
    return committed.feature != identity.feature
        || committed.quality != identity.quality
        || committed.qualityVersion != identity.qualityVersion
        || committed.renderWidth != identity.renderWidth
        || committed.renderHeight != identity.renderHeight
        || committed.outputWidth != identity.outputWidth
        || committed.outputHeight != identity.outputHeight;
}

const ReconstructionJitterSample& ReconstructionJitterState::Prepare(
    const ReconstructionJitterIdentity& identity,
    const bool startsNewHistory)
{
    const std::uint32_t phase = startsNewHistory ? 0u : m_nextPhase;
    m_prepared = MakeSample(
        identity, phase, startsNewHistory, m_previous, m_previousValid);
    m_hasPrepared = true;
    return m_prepared;
}

bool ReconstructionJitterState::ResetThroughHistoryCompatibility()
{
    const bool ownershipConsistent = !m_hasPrepared || m_prepared.phase == 0;
    m_nextPhase = 0;
    m_previous = {};
    m_previousValid = false;
    if (m_hasPrepared)
    {
        m_prepared.previousValid = false;
        m_prepared.startsNewHistory = true;
    }
    return ownershipConsistent;
}

bool ReconstructionJitterState::CommitRendered()
{
    if (!m_hasPrepared)
    {
        return false;
    }
    m_previous = m_prepared;
    m_previous.previousValid = false;
    m_previousValid = true;
    m_nextPhase = (m_prepared.phase + 1u) % m_prepared.period;
    m_hasPrepared = false;
    return true;
}

void ReconstructionJitterState::CancelPrepared()
{
    m_hasPrepared = false;
}

void ReconstructionJitterState::ResetImmediate()
{
    m_prepared = {};
    m_previous = {};
    m_nextPhase = 0;
    m_hasPrepared = false;
    m_previousValid = false;
}
