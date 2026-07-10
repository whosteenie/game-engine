#include "engine/lighting/EnvironmentImportanceSampling.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = glm::pi<float>();

    float Luminance(const float r, const float g, const float b)
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    glm::vec3 SampleEquirectBilinear(
        const std::vector<float>& rgbaRadiance,
        const int width,
        const int height,
        const float u,
        const float v)
    {
        if (width <= 0 || height <= 0 || rgbaRadiance.empty())
        {
            return glm::vec3(0.0f);
        }

        const float clampedU = std::clamp(u, 0.0f, 1.0f);
        const float clampedV = std::clamp(v, 0.0f, 1.0f);
        const float x = clampedU * static_cast<float>(width - 1);
        const float y = clampedV * static_cast<float>(height - 1);

        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);

        const auto sampleTexel = [&](const int texelX, const int texelY) {
            const std::size_t pixelIndex =
                static_cast<std::size_t>(texelY) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(texelX);
            const std::size_t channelIndex = pixelIndex * 4;
            return glm::vec3(
                rgbaRadiance[channelIndex + 0],
                rgbaRadiance[channelIndex + 1],
                rgbaRadiance[channelIndex + 2]);
        };

        const glm::vec3 c00 = sampleTexel(x0, y0);
        const glm::vec3 c10 = sampleTexel(x1, y0);
        const glm::vec3 c01 = sampleTexel(x0, y1);
        const glm::vec3 c11 = sampleTexel(x1, y1);
        const glm::vec3 c0 = glm::mix(c00, c10, tx);
        const glm::vec3 c1 = glm::mix(c01, c11, tx);
        return glm::mix(c0, c1, ty);
    }

    float CosLatitudeForEquirectV(const float v)
    {
        // Matches DirectionToEquirectUv / y = sin(pi*(v-0.5)) in environment_sampling.hlsl.
        return std::max(std::cos(kPi * (v - 0.5f)), 1e-6f);
    }
}

EnvImportanceSamplingBuildResult BuildEquirectEnvImportanceCdf(
    const std::vector<float>& rgbaRadiance,
    const int hdrWidth,
    const int hdrHeight,
    const int maxCdfWidth,
    const int maxCdfHeight)
{
    EnvImportanceSamplingBuildResult result{};
    if (hdrWidth <= 0 || hdrHeight <= 0 || rgbaRadiance.empty())
    {
        return result;
    }

    result.cdfWidth = std::clamp(hdrWidth, 1, std::max(1, maxCdfWidth));
    result.cdfHeight = std::clamp(hdrHeight, 1, std::max(1, maxCdfHeight));
    if (hdrWidth > maxCdfWidth || hdrHeight > maxCdfHeight)
    {
        // Preserve aspect while downscaling.
        const float aspect = static_cast<float>(hdrWidth) / static_cast<float>(hdrHeight);
        if (aspect >= 1.0f)
        {
            result.cdfWidth = maxCdfWidth;
            result.cdfHeight = std::max(1, static_cast<int>(std::lround(maxCdfWidth / aspect)));
        }
        else
        {
            result.cdfHeight = maxCdfHeight;
            result.cdfWidth = std::max(1, static_cast<int>(std::lround(maxCdfHeight * aspect)));
        }
    }

    const std::size_t cellCount =
        static_cast<std::size_t>(result.cdfWidth) * static_cast<std::size_t>(result.cdfHeight);
    std::vector<float> weights(cellCount, 0.0f);

    for (int y = 0; y < result.cdfHeight; ++y)
    {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(result.cdfHeight);
        const float cosLat = CosLatitudeForEquirectV(v);
        for (int x = 0; x < result.cdfWidth; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(result.cdfWidth);
            const glm::vec3 radiance = SampleEquirectBilinear(rgbaRadiance, hdrWidth, hdrHeight, u, v);
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(result.cdfWidth) +
                static_cast<std::size_t>(x);
            weights[index] = Luminance(radiance.r, radiance.g, radiance.b) * cosLat;
        }
    }

    // Clamp hot HDR sun pixels out of the IS table — the analytic sun NEE handles the disk.
    // Without this the CDF over-samples the env sun and fights g_SunDirection (double shadows).
    if (!weights.empty())
    {
        std::vector<float> sorted = weights;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t percentileIndex =
            std::min(sorted.size() - 1, sorted.size() * 95 / 100);
        const float lumClamp = std::max(sorted[percentileIndex], 1.0f);
        for (float& weight : weights)
        {
            weight = std::min(weight, lumClamp);
        }
    }

    result.cdf.resize(cellCount + 1);
    result.cdf[0] = 0.0f;
    float running = 0.0f;
    for (std::size_t i = 0; i < cellCount; ++i)
    {
        running += weights[i];
        result.cdf[i + 1] = running;
    }

    result.weightSum = running;
    if (result.weightSum <= 1e-12f)
    {
        result.cdf.clear();
        result.cdfWidth = 0;
        result.cdfHeight = 0;
        result.weightSum = 0.0f;
        return result;
    }

    const float invSum = 1.0f / result.weightSum;
    for (float& value : result.cdf)
    {
        value *= invSum;
    }
    result.cdf.back() = 1.0f;
    return result;
}
