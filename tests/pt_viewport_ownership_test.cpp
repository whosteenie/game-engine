#include "engine/raytracing/DxrPathTracerDispatch.h"

namespace
{
    void Expect(const bool condition, int& failures)
    {
        if (!condition)
        {
            ++failures;
        }
    }
}

void RunPtViewportOwnershipTests(int& failures)
{
    DxrPathTracerDispatch::ViewportSequenceState scene(0);
    DxrPathTracerDispatch::ViewportSequenceState game(1);

    Expect(scene.GetViewportId() == 0 && game.GetViewportId() == 1, failures);
    Expect(scene.GetAccumulationFrameIndex() == 0, failures);
    Expect(game.GetAccumulationFrameIndex() == 0, failures);

    // Only a successfully rendered frame commits identity for its viewport.
    scene.BeginEvaluation();
    scene.CommitRenderedFrame();
    scene.BeginEvaluation();
    scene.CommitRenderedFrame();
    game.BeginEvaluation();
    game.CommitRenderedFrame();
    Expect(scene.GetAccumulationFrameIndex() == 2, failures);
    Expect(game.GetAccumulationFrameIndex() == 1, failures);

    // A failed/skipped evaluation clears this-evaluation status but does not advance identity.
    game.BeginEvaluation();
    Expect(!game.DispatchedThisFrame(), failures);
    Expect(game.GetAccumulationFrameIndex() == 1, failures);
    Expect(scene.GetAccumulationFrameIndex() == 2, failures);

    // A local reset cannot change the other viewport's sequence.
    scene.ResetAccumulation();
    Expect(scene.GetAccumulationFrameIndex() == 0, failures);
    Expect(game.GetAccumulationFrameIndex() == 1, failures);

    // The production selector rejects unknown identities rather than aliasing Scene or Game.
    Expect(DxrPathTracerDispatch::IsSupportedViewportId(0), failures);
    Expect(DxrPathTracerDispatch::IsSupportedViewportId(1), failures);
    Expect(!DxrPathTracerDispatch::IsSupportedViewportId(2), failures);
}
