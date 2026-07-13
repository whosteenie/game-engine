#pragma once

#include <cstdint>
#include <vector>

struct EnvImportanceSamplingBuildResult
{
    // Monotonic CDF over flattened cdfWidth x cdfHeight cells: cdf[0] = 0, cdf.back() = 1.
    std::vector<float> cdf;
    int cdfWidth = 0;
    int cdfHeight = 0;
    float weightSum = 0.0f;
    // 95th-percentile sky luminance used to remove embedded HDR suns from transport when the
    // renderer supplies an analytic sun. The visible background remains unchanged.
    float directLightingLuminanceClamp = 0.0f;
};

// Luminance-weighted equirect importance table (sin(latitude) solid-angle Jacobian).
// Downsamples to at most maxCdfWidth x maxCdfHeight for a compact GPU CDF.
EnvImportanceSamplingBuildResult BuildEquirectEnvImportanceCdf(
    const std::vector<float>& rgbaRadiance,
    int hdrWidth,
    int hdrHeight,
    int maxCdfWidth = 512,
    int maxCdfHeight = 256);
