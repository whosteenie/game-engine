#include "engine/rendering/HistoryCompatibility.h"

#include <cstdint>
#include <iostream>

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

    // S1-P5 conservative transmission matrix. Streamline exposes a viewport reset here but no
    // documented per-pixel rejection input, so an optical generation change rejects only the
    // reconstruction/display-bloom owners of the affected viewport. Camera-only motion retains
    // reuse because it does not change the scene or instance-transform generations.
    HistoryCompatibilityKey transmission = base;
    transmission.producer = HistoryRenderProducer::PathTracer;
    transmission.guideProducer = HistoryGuideProducer::PathTracer;
    transmission.feature = HistoryReconstructionFeature::RayReconstruction;
    transmission.opticalSceneVersion = 11;
    transmission.opticalMotionVersion = 29;
    HistoryCompatibilityState transmissionState = SeededState(transmission, failures);
    const HistoryCompatibilityTransition cameraOnly = transmissionState.Begin(transmission);
    Expect(cameraOnly.IsCompatible() && !cameraOnly.scheduled, failures);
    std::cout << "S1-P5 case=camera-only result=reuse optical_scene=11 optical_motion=29\n";
    transmissionState.CancelPending();
    int staticReuseFrames = 0;
    for (int frame = 0; frame < 64; ++frame)
    {
        const HistoryCompatibilityTransition steady = transmissionState.Begin(transmission);
        if (steady.IsCompatible() && !steady.scheduled)
        {
            ++staticReuseFrames;
        }
        transmissionState.CancelPending();
    }
    Expect(staticReuseFrames == 64, failures);
    std::cout << "S1-P5 static-reuse frames=" << staticReuseFrames << "/64 resets=0\n";

    auto expectOpticalSceneReject = [&](const char* caseName, HistoryCompatibilityKey changed) {
        HistoryCompatibilityState state = SeededState(transmission, failures);
        const HistoryCompatibilityTransition rejected = state.Begin(changed);
        Expect(rejected.scheduled, failures);
        Expect(rejected.reasonBits == OpticalDomain, failures);
        Expect(rejected.ownerBits == (Reconstruction | DisplayBloom), failures);
        std::cout << "S1-P5 case=" << caseName
                  << " result=reject reason_bits=" << rejected.reasonBits
                  << " owner_bits=" << rejected.ownerBits
                  << " optical_scene=" << changed.opticalSceneVersion
                  << " optical_motion=" << changed.opticalMotionVersion << "\n";
    };

    // Motion is resolved by the PT motion guide and per-pixel surface validation. It must not
    // reset the full viewport history just because some other instance is moving.
    HistoryCompatibilityKey movingChecker = transmission;
    ++movingChecker.opticalMotionVersion;
    Expect(transmissionState.Begin(movingChecker).IsCompatible(), failures);
    transmissionState.CancelPending();
    HistoryCompatibilityKey movingPane = transmission;
    ++movingPane.opticalMotionVersion;
    Expect(transmissionState.Begin(movingPane).IsCompatible(), failures);
    transmissionState.CancelPending();
    HistoryCompatibilityKey movingReceiver = transmission;
    ++movingReceiver.opticalMotionVersion;
    Expect(transmissionState.Begin(movingReceiver).IsCompatible(), failures);
    transmissionState.CancelPending();
    std::cout << "S1-P5 moving transforms result=per-pixel reuse\n";

    // Replacement/topology/material edits are also outside the previous optical domain.
    HistoryCompatibilityKey replacedBackground = transmission;
    ++replacedBackground.opticalSceneVersion;
    expectOpticalSceneReject("replaced-background", replacedBackground);

    // An opaque-only PT scene keeps zero optical generations, so unrelated instance motion does
    // not enter this policy.
    HistoryCompatibilityKey opaquePt = transmission;
    opaquePt.opticalSceneVersion = 0;
    opaquePt.opticalMotionVersion = 0;
    HistoryCompatibilityState opaqueState = SeededState(opaquePt, failures);
    Expect(opaqueState.Begin(opaquePt).IsCompatible(), failures);

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
