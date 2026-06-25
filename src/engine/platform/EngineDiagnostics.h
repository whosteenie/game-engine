#pragma once

#include <string>

namespace EngineDiagnostics
{
    void SetLastGpuAllocationError(const std::string& message);
    std::string GetLastGpuAllocationError();
}
