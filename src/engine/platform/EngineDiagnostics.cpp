#include "engine/platform/EngineDiagnostics.h"

namespace
{
    thread_local std::string g_lastGpuAllocationError;
}

namespace EngineDiagnostics
{
    void SetLastGpuAllocationError(const std::string& message)
    {
        g_lastGpuAllocationError = message;
    }

    std::string GetLastGpuAllocationError()
    {
        return g_lastGpuAllocationError;
    }
}
