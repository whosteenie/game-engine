#pragma once

#include <cstdint>
#include <string>

struct DxrDiagnostics
{
    std::uint32_t blasCount = 0;
    std::uint32_t tlasInstanceCount = 0;
    std::uint64_t totalRtTriangles = 0;
    std::uint64_t asGpuMemoryBytes = 0;
    std::string buildStatus = "SKIPPED (RT off)";
    double lastBuildTimeMs = 0.0;
};
