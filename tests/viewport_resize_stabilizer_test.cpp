#include "app/editor/ViewportResizeStabilizer.h"

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

void RunViewportResizeStabilizerTests(int& failures)
{
    using Decision = ViewportResizeStabilizer::Decision;

    ViewportResizeStabilizer state;
    Expect(state.Update(0, 0, 1280, 720, 0.0) == Decision::Commit, failures);
    Expect(!state.IsPending(), failures);
    Expect(state.Update(1280, 720, 1281, 719, 1.0) == Decision::Stable, failures);

    Expect(state.Update(1280, 720, 1400, 800, 2.0) == Decision::Pending, failures);
    Expect(state.IsPending(), failures);
    Expect(state.Update(1280, 720, 1400, 800, 2.14) == Decision::Pending, failures);
    Expect(state.Update(1280, 720, 1400, 800, 2.151) == Decision::Commit, failures);
    Expect(!state.IsPending(), failures);

    // Every materially new drag size restarts the quiet period.
    Expect(state.Update(1400, 800, 1500, 850, 3.0) == Decision::Pending, failures);
    Expect(state.Update(1400, 800, 1600, 900, 3.10) == Decision::Pending, failures);
    Expect(state.Update(1400, 800, 1600, 900, 3.20) == Decision::Pending, failures);
    Expect(state.Update(1400, 800, 1600, 900, 3.251) == Decision::Commit, failures);

    // Returning to the committed extent cancels an in-progress resize without a rebuild.
    Expect(state.Update(1600, 900, 1700, 950, 4.0) == Decision::Pending, failures);
    Expect(state.Update(1600, 900, 1600, 900, 4.01) == Decision::Stable, failures);
    Expect(!state.IsPending(), failures);

    // A backward/reset clock cannot accidentally commit a pending extent.
    Expect(state.Update(1600, 900, 1800, 1000, 5.0) == Decision::Pending, failures);
    Expect(state.Update(1600, 900, 1800, 1000, 1.0) == Decision::Pending, failures);

    // Viewport instances have independent pending identities.
    ViewportResizeStabilizer secondViewport;
    Expect(secondViewport.Update(800, 600, 900, 700, 5.0) == Decision::Pending, failures);
    Expect(state.IsPending() && secondViewport.IsPending(), failures);
    secondViewport.Reset();
    Expect(state.IsPending() && !secondViewport.IsPending(), failures);
}
