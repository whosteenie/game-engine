#include "engine/rendering/HistoryCompatibility.h"

#include <cstdint>

namespace
{
    void Expect(const bool condition, int& failures)
    {
        if (!condition)
        {
            ++failures;
        }
    }

    HistoryCompatibilityKey BaseKey()
    {
        HistoryCompatibilityKey key{};
        key.producer = HistoryRenderProducer::Raster;
        key.guideProducer = HistoryGuideProducer::Raster;
        key.guideVersion = 1;
        key.feature = HistoryReconstructionFeature::Dlss;
        key.quality = HistoryReconstructionQuality::Dlaa;
        key.renderWidth = 2560;
        key.renderHeight = 1440;
        key.outputWidth = 2560;
        key.outputHeight = 1440;
        key.cameraPacketValid = true;
        return key;
    }

    HistoryCompatibilityState SeededState(const HistoryCompatibilityKey& key, int& failures)
    {
        HistoryCompatibilityState state;
        const HistoryCompatibilityTransition first = state.Begin(key);
        Expect(first.scheduled, failures);
        Expect((first.reasonBits & HistoryCompatibilityReason::FirstFrame) != 0, failures);
        Expect(first.ownerBits == HistoryCompatibilityOwner::All, failures);
        Expect(state.CommitRendered(), failures);
        return state;
    }

    void ExpectOneTransition(
        const HistoryCompatibilityKey& before,
        const HistoryCompatibilityKey& after,
        const std::uint32_t expectedReasons,
        const std::uint32_t expectedOwners,
        int& failures)
    {
        HistoryCompatibilityState state = SeededState(before, failures);
        const HistoryCompatibilityTransition transition = state.Begin(after);
        Expect(transition.scheduled, failures);
        Expect(transition.reasonBits == expectedReasons, failures);
        Expect(transition.ownerBits == expectedOwners, failures);

        // Repeating pre-render setup cannot schedule the same reset again.
        const HistoryCompatibilityTransition duplicate = state.Begin(after);
        Expect(!duplicate.scheduled && duplicate.ownerBits == 0, failures);
        Expect(!state.HasCommittedKey() || state.CommittedKey() == before, failures);

        Expect(state.CommitRendered(), failures);
        HistoryCompatibilityKey steady = after;
        steady.cameraPacketValid = true;
        steady.cameraCut = false;
        const HistoryCompatibilityTransition compatible = state.Begin(steady);
        Expect(compatible.IsCompatible(), failures);
        Expect(!compatible.scheduled && compatible.ownerBits == 0, failures);
    }
}

void RunHistoryCompatibilityTests(int& failures)
{
    using namespace HistoryCompatibilityOwner;
    using namespace HistoryCompatibilityReason;

    const HistoryCompatibilityKey base = BaseKey();

    // First frame resets once and cannot commit identity until rendering is explicitly reported.
    HistoryCompatibilityState firstFrame;
    const HistoryCompatibilityTransition first = firstFrame.Begin(base);
    Expect(first.reasonBits == FirstFrame && first.ownerBits == All, failures);
    Expect(!firstFrame.HasCommittedKey(), failures);
    firstFrame.CancelPending();
    Expect(!firstFrame.HasCommittedKey() && !firstFrame.HasPendingKey(), failures);
    const HistoryCompatibilityTransition retriedFirst = firstFrame.Begin(base);
    Expect(retriedFirst.scheduled && retriedFirst.reasonBits == FirstFrame, failures);
    Expect(firstFrame.CommitRendered(), failures);
    Expect(firstFrame.Begin(base).IsCompatible(), failures);

    // Raster -> hybrid -> PT are distinct producer identities, and each transition resets all
    // affected owners once.
    HistoryCompatibilityKey hybrid = base;
    hybrid.producer = HistoryRenderProducer::Hybrid;
    ExpectOneTransition(base, hybrid, Producer, All, failures);
    HistoryCompatibilityKey pathTraced = hybrid;
    pathTraced.producer = HistoryRenderProducer::PathTracer;
    pathTraced.guideProducer = HistoryGuideProducer::PathTracer;
    ExpectOneTransition(hybrid, pathTraced, Producer | Guide, All, failures);

    // DLSS/RR feature switches reset at identical quality and extent.
    HistoryCompatibilityKey rr = base;
    rr.feature = HistoryReconstructionFeature::RayReconstruction;
    ExpectOneTransition(
        base, rr, Feature, Reconstruction | DisplayBloom, failures);
    HistoryCompatibilityKey taa = base;
    taa.feature = HistoryReconstructionFeature::Taa;
    taa.quality = HistoryReconstructionQuality::Taa;
    ExpectOneTransition(
        taa,
        base,
        Feature | Quality,
        Reconstruction | RenderBloom | DisplayBloom,
        failures);

    // DLAA/SR quality and RR model changes are separate quality identities.
    HistoryCompatibilityKey sr = base;
    sr.quality = HistoryReconstructionQuality::Performance;
    ExpectOneTransition(
        base, sr, Quality, Reconstruction | DisplayBloom, failures);
    HistoryCompatibilityKey rrPreset = rr;
    rrPreset.qualityVersion = 2;
    ExpectOneTransition(
        rr, rrPreset, Quality, Reconstruction | DisplayBloom, failures);

    // Guide producer/version and diagnostic bundle changes reset only guide consumers.
    HistoryCompatibilityKey guide = base;
    guide.guideProducer = HistoryGuideProducer::Mixed;
    ExpectOneTransition(
        base, guide, Guide, Reconstruction | DisplayBloom, failures);
    HistoryCompatibilityKey guideVersion = base;
    guideVersion.guideVersion = 2;
    ExpectOneTransition(
        base, guideVersion, Guide, Reconstruction | DisplayBloom, failures);
    HistoryCompatibilityKey diagnostic = base;
    diagnostic.diagnosticSignal = 3;
    ExpectOneTransition(
        base, diagnostic, DiagnosticSignal, Reconstruction | DisplayBloom, failures);

    // Render extent affects every owner; output-only resize affects reconstruction/display bloom.
    HistoryCompatibilityKey renderResize = base;
    renderResize.renderWidth = 1920;
    renderResize.renderHeight = 1080;
    ExpectOneTransition(base, renderResize, RenderExtent, All, failures);
    HistoryCompatibilityKey outputResize = base;
    outputResize.outputWidth = 1920;
    outputResize.outputHeight = 1080;
    ExpectOneTransition(
        base, outputResize, OutputExtent, Reconstruction | DisplayBloom, failures);

    // Invalid/cut camera events reset once. Commit normalizes the event, so the next valid frame is
    // compatible rather than a second reset on the false->true validity edge.
    HistoryCompatibilityKey invalidCamera = base;
    invalidCamera.cameraPacketValid = false;
    ExpectOneTransition(base, invalidCamera, CameraInvalid, All, failures);
    HistoryCompatibilityKey cutCamera = base;
    cutCamera.cameraCut = true;
    ExpectOneTransition(base, cutCamera, CameraCut, All, failures);

    // Two independent viewport states cannot observe or commit one another's transition.
    HistoryCompatibilityState scene = SeededState(base, failures);
    HistoryCompatibilityState game = SeededState(base, failures);
    const HistoryCompatibilityTransition sceneTransition = scene.Begin(pathTraced);
    Expect(sceneTransition.scheduled, failures);
    Expect(game.Begin(base).IsCompatible(), failures);
    Expect(scene.CommitRendered(), failures);
    Expect(game.CommittedKey() == base, failures);
}
