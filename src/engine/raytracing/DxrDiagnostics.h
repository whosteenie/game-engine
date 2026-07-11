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
    // Emissive-triangle NEE table (refreshed every PT frame by UploadEmissiveLights). Count 0 means
    // NEE toward emitters is inactive — the PtIsolateDirectEmissive ("emissive NEE") view is then
    // legitimately black and any emitter contribution comes only from BSDF-sampled surface hits.
    std::uint32_t emissiveLightCount = 0;
    std::uint32_t emissiveTriangleCount = 0;
    float emissiveLightPickWeightSum = 0.0f;
};
