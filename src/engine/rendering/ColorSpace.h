#pragma once

#include <glm/glm.hpp>

#include <cmath>

namespace ColorSpace
{
    inline float SrgbChannelToLinear(const float channel)
    {
        if (channel <= 0.04045f)
        {
            return channel / 12.92f;
        }

        return std::pow((channel + 0.055f) / 1.055f, 2.4f);
    }

    inline float LinearChannelToSrgb(const float channel)
    {
        if (channel <= 0.0031308f)
        {
            return channel * 12.92f;
        }

        return 1.055f * std::pow(channel, 1.0f / 2.4f) - 0.055f;
    }

    inline glm::vec3 SrgbToLinear(const glm::vec3& srgb)
    {
        return glm::vec3(
            SrgbChannelToLinear(srgb.r),
            SrgbChannelToLinear(srgb.g),
            SrgbChannelToLinear(srgb.b));
    }

    inline glm::vec3 LinearToSrgb(const glm::vec3& linear)
    {
        return glm::vec3(
            LinearChannelToSrgb(linear.r),
            LinearChannelToSrgb(linear.g),
            LinearChannelToSrgb(linear.b));
    }
}
