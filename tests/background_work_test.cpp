#include "engine/platform/system/BackgroundWork.h"

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

void RunBackgroundWorkTests(int& failures)
{
    Expect(BackgroundWork::ResponsiveWorkerCount(0, 16) == 0, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 0) == 1, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 2) == 1, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 8) == 4, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 16) == 6, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(3, 16) == 3, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 16, 2) == 2, failures);
    Expect(BackgroundWork::ResponsiveWorkerCount(20, 16, 0) == 0, failures);
}
