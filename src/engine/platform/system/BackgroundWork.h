#pragma once

#include <cstddef>

namespace BackgroundWork
{
    // Chooses bounded parallelism for latency-insensitive startup work. Keep at least half of the
    // reported logical processors available to the desktop, driver, and application's UI thread.
    std::size_t ResponsiveWorkerCount(
        std::size_t taskCount,
        unsigned int hardwareConcurrency,
        std::size_t maximumWorkers = 6);

    // Best-effort platform hint. Failure leaves the current thread at its existing priority.
    void LowerCurrentThreadPriority();
}
