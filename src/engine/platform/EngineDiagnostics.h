#pragma once

#include <string>

namespace EngineDiagnostics
{
    void SetLastGpuAllocationError(const std::string& message);
    void ClearLastGpuAllocationError();
    std::string GetLastGpuAllocationError();
}
