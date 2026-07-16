#include "engine/rendering/ReconstructionJitter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <utility>

namespace
{
    void Expect(const bool condition, int& failures)
    {
        if (!condition)
        {
            ++failures;
        }
    }

    bool Near(const float lhs, const float rhs, const float tolerance = 1.0e-6f)
    {
        return std::abs(lhs - rhs) <= tolerance;
    }

    ReconstructionJitterIdentity Identity(
        const HistoryReconstructionFeature feature,
        const HistoryReconstructionQuality quality,
        const int renderWidth = 1280,
        const int renderHeight = 720,
        const int outputWidth = 1920,
        const int outputHeight = 1080)
    {
        ReconstructionJitterIdentity identity{};
        identity.feature = feature;
        identity.quality = quality;
        identity.renderWidth = renderWidth;
        identity.renderHeight = renderHeight;
        identity.outputWidth = outputWidth;
        identity.outputHeight = outputHeight;
        return identity;
    }

    HistoryCompatibilityKey HistoryKey(const ReconstructionJitterIdentity& identity)
    {
        HistoryCompatibilityKey key{};
        key.feature = identity.feature;
        key.quality = identity.quality;
        key.qualityVersion = identity.qualityVersion;
        key.renderWidth = identity.renderWidth;
        key.renderHeight = identity.renderHeight;
        key.outputWidth = identity.outputWidth;
        key.outputHeight = identity.outputHeight;
        key.cameraPacketValid = true;
        return key;
    }

    HistoryCompatibilityState SeedHistory(
        const ReconstructionJitterIdentity& identity,
        int& failures)
    {
        HistoryCompatibilityState history;
        Expect(history.Begin(HistoryKey(identity)).scheduled, failures);
        Expect(history.CommitRendered(), failures);
        return history;
    }

    void ExpectFullCycle(
        const ReconstructionJitterIdentity& identity,
        const std::uint32_t expectedPeriod,
        const char* featureName,
        const char* qualityName,
        int& failures)
    {
        ReconstructionJitterState state;
        std::set<std::pair<float, float>> uniqueSamples;
        for (std::uint32_t frame = 0; frame < expectedPeriod; ++frame)
        {
            const bool reset = frame == 0;
            const ReconstructionJitterSample& sample = state.Prepare(identity, reset);
            if (reset)
            {
                Expect(state.ResetThroughHistoryCompatibility(), failures);
            }
            Expect(sample.period == expectedPeriod, failures);
            Expect(sample.phase == frame, failures);
            Expect(sample.previousValid == (frame != 0), failures);
            if (frame != 0)
            {
                Expect(sample.previousPhase == frame - 1, failures);
            }
            Expect(std::abs(sample.xNdc * static_cast<float>(identity.renderWidth)) <= 1.0f,
                failures);
            Expect(std::abs(sample.yNdc * static_cast<float>(identity.renderHeight)) <= 1.0f,
                failures);
            uniqueSamples.emplace(sample.xNdc, sample.yNdc);
            Expect(state.CommitRendered(), failures);
        }
        Expect(uniqueSamples.size() == expectedPeriod, failures);

        const ReconstructionJitterSample& wrapped = state.Prepare(identity, false);
        Expect(wrapped.phase == 0, failures);
        Expect(wrapped.previousValid && wrapped.previousPhase == expectedPeriod - 1, failures);
        std::cout << "S2-P3 tuple feature=" << featureName
                  << " quality=" << qualityName
                  << " period=" << expectedPeriod
                  << " trace=0.." << (expectedPeriod - 1) << "->0"
                  << " previous=" << wrapped.previousPhase << " result=PASS\n";
    }
}

void RunReconstructionJitterTests(int& failures)
{
    using Feature = HistoryReconstructionFeature;
    using Quality = HistoryReconstructionQuality;

    struct DlssTuple
    {
        Quality quality;
        std::uint32_t period;
        const char* name;
    };
    constexpr std::array<DlssTuple, 5> dlssTuples{{
        {Quality::Dlaa, 8, "dlaa"},
        {Quality::Quality, 18, "quality"},
        {Quality::Balanced, 24, "balanced"},
        {Quality::Performance, 32, "performance"},
        {Quality::UltraPerformance, 72, "ultra-performance"},
    }};
    for (const DlssTuple& tuple : dlssTuples)
    {
        ExpectFullCycle(Identity(Feature::Dlss, tuple.quality), tuple.period,
            "dlss", tuple.name, failures);
    }

    constexpr std::array<Quality, 5> rrQualities{
        Quality::Dlaa,
        Quality::Quality,
        Quality::Balanced,
        Quality::Performance,
        Quality::UltraPerformance,
    };
    constexpr std::array<const char*, 5> rrNames{
        "dlaa", "quality", "balanced", "performance", "ultra-performance"};
    for (std::size_t i = 0; i < rrQualities.size(); ++i)
    {
        ExpectFullCycle(Identity(Feature::RayReconstruction, rrQualities[i]), 64,
            "rr", rrNames[i], failures);
    }

    // Preserve the existing Halton sign and NDC scale. Phase zero is (-1/width, -1/height), and
    // phase one keeps x centered while the base-3 component remains negative.
    ReconstructionJitterState motionConvention;
    const ReconstructionJitterIdentity native =
        Identity(Feature::Dlss, Quality::Dlaa, 800, 600, 800, 600);
    const ReconstructionJitterSample phaseZero = motionConvention.Prepare(native, true);
    Expect(Near(phaseZero.xNdc, -1.0f / 800.0f), failures);
    Expect(Near(phaseZero.yNdc, -1.0f / 600.0f), failures);
    Expect(motionConvention.ResetThroughHistoryCompatibility(), failures);
    Expect(motionConvention.CommitRendered(), failures);
    const ReconstructionJitterSample phaseOne = motionConvention.Prepare(native, false);
    Expect(Near(phaseOne.xNdc, 0.0f), failures);
    Expect(Near(phaseOne.yNdc, (-1.0f / 3.0f) / 600.0f), failures);
    motionConvention.CancelPrepared();

    // Each viewport owns an independent phase. Preparing and then skipping a viewport cannot
    // advance it or steal the other viewport's previous sample.
    ReconstructionJitterState sceneView;
    ReconstructionJitterState gameView;
    const ReconstructionJitterIdentity quality = Identity(Feature::Dlss, Quality::Quality);
    sceneView.Prepare(quality, true);
    Expect(sceneView.ResetThroughHistoryCompatibility() && sceneView.CommitRendered(), failures);
    gameView.Prepare(quality, true);
    Expect(gameView.ResetThroughHistoryCompatibility(), failures);
    gameView.CancelPrepared();
    const ReconstructionJitterSample scenePhaseOne = sceneView.Prepare(quality, false);
    Expect(scenePhaseOne.phase == 1 && scenePhaseOne.previousPhase == 0, failures);
    Expect(sceneView.CommitRendered(), failures);
    const ReconstructionJitterSample gameStillZero = gameView.Prepare(quality, true);
    Expect(gameStillZero.phase == 0 && !gameStillZero.previousValid, failures);
    Expect(gameView.ResetThroughHistoryCompatibility() && gameView.CommitRendered(), failures);
    Expect(sceneView.NextPhase() == 2 && gameView.NextPhase() == 1, failures);
    std::cout << "S2-P3 dual-skipped scene=0,1 next=2 game=skip,0 next=1 result=PASS\n";

    // The preview uses the same S1-owned feature/quality/extent/camera fields that reset jitter.
    const ReconstructionJitterIdentity base = Identity(Feature::Dlss, Quality::Quality);
    const HistoryCompatibilityState history = SeedHistory(base, failures);
    Expect(!ReconstructionJitterNeedsPhaseZero(history, base, true, false), failures);
    ReconstructionJitterIdentity changed = base;
    changed.feature = Feature::RayReconstruction;
    Expect(ReconstructionJitterNeedsPhaseZero(history, changed, true, false), failures);
    changed = base;
    changed.quality = Quality::UltraPerformance;
    Expect(ReconstructionJitterNeedsPhaseZero(history, changed, true, false), failures);
    changed = base;
    changed.qualityVersion = 1;
    Expect(ReconstructionJitterNeedsPhaseZero(history, changed, true, false), failures);
    changed = base;
    ++changed.renderWidth;
    Expect(ReconstructionJitterNeedsPhaseZero(history, changed, true, false), failures);
    changed = base;
    ++changed.outputHeight;
    Expect(ReconstructionJitterNeedsPhaseZero(history, changed, true, false), failures);
    Expect(ReconstructionJitterNeedsPhaseZero(history, base, false, false), failures);
    Expect(ReconstructionJitterNeedsPhaseZero(history, base, true, true), failures);

    ReconstructionJitterState transition;
    transition.Prepare(base, true);
    Expect(transition.ResetThroughHistoryCompatibility() && transition.CommitRendered(), failures);
    transition.Prepare(base, false);
    Expect(transition.CommitRendered(), failures);
    const ReconstructionJitterSample resetFrame = transition.Prepare(changed, true);
    Expect(resetFrame.phase == 0 && !resetFrame.previousValid, failures);
    Expect(transition.ResetThroughHistoryCompatibility(), failures);
    Expect(transition.CommitRendered(), failures);
    const ReconstructionJitterSample afterReset = transition.Prepare(changed, false);
    Expect(afterReset.phase == 1 && afterReset.previousValid && afterReset.previousPhase == 0,
        failures);
    std::cout << "S2-P3 transitions feature,quality,model,render-extent,output-extent,"
                 "camera-invalid,camera-cut reset=0 next=1 previous=0 result=PASS\n";
}
