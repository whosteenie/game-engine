#include "app/project/ProjectViewportRevealGate.h"

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

void RunProjectViewportRevealGateTests(int& failures)
{
    Expect(!CanRevealProjectEditor({}), failures);

    // A stable layout on a non-viewport tab may reveal immediately.
    Expect(CanRevealProjectEditor({true}), failures);

    // Scene View remains behind the project presentation until its first image is composited.
    Expect(!CanRevealProjectEditor({true, true, false, false, false}), failures);
    Expect(CanRevealProjectEditor({true, true, true, false, false}), failures);

    // Game View with a camera has the same image requirement. Without a camera, callers leave
    // gameImageRequired false so its intentional "No camera" placeholder can be revealed.
    Expect(!CanRevealProjectEditor({true, false, false, true, false}), failures);
    Expect(CanRevealProjectEditor({true, false, false, true, true}), failures);
    Expect(CanRevealProjectEditor({true, false, false, false, false}), failures);

    // If both viewport tabs are independently visible, both images must be ready.
    Expect(!CanRevealProjectEditor({true, true, true, true, false}), failures);
    Expect(!CanRevealProjectEditor({true, true, false, true, true}), failures);
    Expect(CanRevealProjectEditor({true, true, true, true, true}), failures);
}
