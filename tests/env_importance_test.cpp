// CPU regression for environment importance CDF build (S5 step 13 / F2).

#include "engine/lighting/environment/Importance.h"
#include "test_expect.h"

#include <cmath>
#include <vector>

void RunEnvImportanceTests()
{
    // Single bright texel on the equator should dominate sampling weight.
    std::vector<float> rgba(4 * 8 * 4, 0.0f);
    const int width = 8;
    const int height = 4;
    const int brightX = 2;
    // Place on the equator (v = 0.5) so sin(lat) weighting does not skew the peak cell.
    const int brightY = 1;
    const std::size_t brightIndex =
        (static_cast<std::size_t>(brightY) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(brightX))
        * 4;
    rgba[brightIndex + 0] = 3.0f;
    rgba[brightIndex + 1] = 3.0f;
    rgba[brightIndex + 2] = 3.0f;
    rgba[brightIndex + 3] = 1.0f;

    const EnvImportanceSamplingBuildResult build =
        BuildEquirectEnvImportanceCdf(rgba, width, height, width, height);
    test::ExpectTrue(build.cdfWidth == width, "CDF keeps requested width");
    test::ExpectTrue(build.cdfHeight == height, "CDF keeps requested height");
    test::ExpectTrue(build.cdf.size() == static_cast<std::size_t>(width * height + 1), "CDF length");
    test::ExpectNear(build.cdf.front(), 0.0f, 1e-6f, "CDF starts at 0");
    test::ExpectNear(build.cdf.back(), 1.0f, 1e-6f, "CDF ends at 1");

    bool monotonic = true;
    for (std::size_t i = 1; i < build.cdf.size(); ++i)
    {
        monotonic = monotonic && (build.cdf[i] >= build.cdf[i - 1]);
    }
    test::ExpectTrue(monotonic, "CDF is monotonic");

    const std::size_t brightCell =
        static_cast<std::size_t>(brightY) * static_cast<std::size_t>(width) +
        static_cast<std::size_t>(brightX);

    float maxDelta = 0.0f;
    std::size_t maxCell = 0;
    for (std::size_t cell = 0; cell < static_cast<std::size_t>(width * height); ++cell)
    {
        const float delta = build.cdf[cell + 1] - build.cdf[cell];
        if (delta > maxDelta)
        {
            maxDelta = delta;
            maxCell = cell;
        }
    }
    test::ExpectTrue(maxCell == brightCell, "Bright equatorial texel has the largest IS weight");
    test::ExpectTrue(
        maxDelta > 1.0f / static_cast<float>(width * height),
        "Bright texel IS weight exceeds uniform");

    // Preserve the full HDR ratio in the sampling probabilities. Clamping the hot-cell weight
    // while evaluating unclamped radiance severely undersamples that cell and creates fireflies.
    std::vector<float> highDynamicRange(static_cast<std::size_t>(width * height * 4), 1.0f);
    const std::size_t hotCell = static_cast<std::size_t>(brightY * width + brightX);
    highDynamicRange[hotCell * 4 + 0] = 1000.0f;
    highDynamicRange[hotCell * 4 + 1] = 1000.0f;
    highDynamicRange[hotCell * 4 + 2] = 1000.0f;

    const EnvImportanceSamplingBuildResult hdrBuild =
        BuildEquirectEnvImportanceCdf(highDynamicRange, width, height, width, height);
    const std::size_t neighborCell = hotCell + 1;
    const float hotProbability = hdrBuild.cdf[hotCell + 1] - hdrBuild.cdf[hotCell];
    const float neighborProbability =
        hdrBuild.cdf[neighborCell + 1] - hdrBuild.cdf[neighborCell];
    test::ExpectNear(
        hotProbability / neighborProbability,
        1000.0f,
        0.5f,
        "Env CDF preserves HDR radiance ratio");
    test::ExpectNear(
        hdrBuild.directLightingLuminanceClamp,
        1.0f,
        1e-6f,
        "Embedded-sun transport clamp follows ordinary sky luminance");

    const EnvImportanceSamplingBuildResult empty =
        BuildEquirectEnvImportanceCdf({}, 0, 0);
    test::ExpectTrue(empty.cdf.empty(), "Empty HDR yields no CDF");
}
