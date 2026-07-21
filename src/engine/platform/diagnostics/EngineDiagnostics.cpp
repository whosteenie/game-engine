#include "engine/platform/diagnostics/EngineDiagnostics.h"

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

    void ClearLastGpuAllocationError()
    {
        g_lastGpuAllocationError.clear();
    }

    std::string GetLastGpuAllocationError()
    {
        return g_lastGpuAllocationError;
    }
}
