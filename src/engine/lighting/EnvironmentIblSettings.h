#pragma once

#include <algorithm>
#include <cstdint>

// IBL cubemap face resolution. Sky background samples the HDR equirect at native resolution;
// this setting controls reflection / specular IBL cubemap generation only.
enum class EnvironmentIblCubemapResolution : std::uint32_t
{
    Auto = 0,
    Size512 = 512,
    Size1024 = 1024,
    Size2048 = 2048,
    Size4096 = 4096,
};

inline std::uint32_t RoundDownPow2(std::uint32_t value)
{
    if (value <= 1)
    {
        return 1;
    }

    std::uint32_t power = 1;
    while (power * 2u <= value)
    {
        power *= 2u;
    }

    return power;
}

inline std::uint32_t ResolveEnvironmentCubemapFaceResolution(
    EnvironmentIblCubemapResolution mode,
    int hdrWidth,
    int hdrHeight)
{
    if (mode != EnvironmentIblCubemapResolution::Auto)
    {
        return static_cast<std::uint32_t>(mode);
    }

    const int sourceHeight = std::max(hdrHeight, 1);
    const int sourceWidth = std::max(hdrWidth, 1);
    // Equirect height maps to cubemap face size; width is 2:1 for full spheres.
    const int derived = std::max(sourceHeight, sourceWidth / 2);
    const std::uint32_t clamped =
        static_cast<std::uint32_t>(std::clamp(derived, 512, 4096));
    return RoundDownPow2(clamped);
}
