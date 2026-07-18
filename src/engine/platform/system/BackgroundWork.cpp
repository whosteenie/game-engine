#include "engine/platform/system/BackgroundWork.h"

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace BackgroundWork
{
    std::size_t ResponsiveWorkerCount(
        const std::size_t taskCount,
        const unsigned int hardwareConcurrency,
        const std::size_t maximumWorkers)
    {
        if (taskCount == 0 || maximumWorkers == 0)
        {
            return 0;
        }

        const std::size_t logicalProcessorCount = std::max<std::size_t>(1, hardwareConcurrency);
        const std::size_t responsiveBudget = std::max<std::size_t>(1, logicalProcessorCount / 2);
        return std::min({taskCount, maximumWorkers, responsiveBudget});
    }

    void LowerCurrentThreadPriority()
    {
#ifdef _WIN32
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    }
}
