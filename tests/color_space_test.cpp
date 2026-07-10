#include "engine/rendering/ColorSpace.h"

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>

namespace
{
    bool NearlyEqual(const float left, const float right, const float epsilon = 0.001f)
    {
        return std::fabs(left - right) <= epsilon;
    }

    void ExpectTrue(int& failures, const bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }
}

void RunColorSpaceTests(int& failures)
{
    ExpectTrue(
        failures,
        NearlyEqual(ColorSpace::SrgbChannelToLinear(0.0f), 0.0f),
        "sRGB black stays black");
    ExpectTrue(
        failures,
        NearlyEqual(ColorSpace::SrgbChannelToLinear(1.0f), 1.0f, 0.01f),
        "sRGB white stays white");
    ExpectTrue(
        failures,
        ColorSpace::SrgbChannelToLinear(0.5f) < 0.25f,
        "mid sRGB gray converts darker in linear");

    const glm::vec3 srgbGray(0.5f);
    const glm::vec3 linearGray = ColorSpace::SrgbToLinear(srgbGray);
    const glm::vec3 roundTrip = ColorSpace::LinearToSrgb(linearGray);
    ExpectTrue(failures, NearlyEqual(roundTrip.r, srgbGray.r), "sRGB round trip red");
    ExpectTrue(failures, NearlyEqual(roundTrip.g, srgbGray.g), "sRGB round trip green");
    ExpectTrue(failures, NearlyEqual(roundTrip.b, srgbGray.b), "sRGB round trip blue");
}
