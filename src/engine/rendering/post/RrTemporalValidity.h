#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RrTemporalValidity
{
    inline constexpr std::uint32_t SurfaceDomain = 0x51f2e3d1u;
    inline constexpr std::uint32_t SkyDomain = 0x8ad42f39u;
    inline constexpr std::uint32_t MirrorDomain = 0xd1b54a35u;

    inline std::uint32_t Mix(std::uint32_t owner, const std::uint32_t value)
    {
        owner ^= value + 0x9e3779b9u + (owner << 6u) + (owner >> 2u);
        owner ^= owner >> 16u;
        owner *= 0x7feb352du;
        owner ^= owner >> 15u;
        return owner == 0u ? 1u : owner;
    }

    inline std::uint32_t SurfaceOwner(const std::uint32_t instanceId)
    {
        return Mix(SurfaceDomain, instanceId + 1u);
    }

    struct Uv
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    inline Uv Reproject(
        const Uv currentUv,
        const Uv motionNdc,
        const Uv motionScale = {-0.5f, 0.5f},
        const Uv currentJitterNdc = {},
        const Uv previousJitterNdc = {})
    {
        return {
            currentUv.x + motionNdc.x * motionScale.x
                + (previousJitterNdc.x - currentJitterNdc.x) * 0.5f,
            currentUv.y + motionNdc.y * motionScale.y
                - (previousJitterNdc.y - currentJitterNdc.y) * 0.5f};
    }

    enum Rejection : std::uint32_t
    {
        None = 0,
        InvalidHistoryOrUv = 1u << 0u,
        OwnerMismatch = 1u << 1u,
        DepthMismatch = 1u << 2u,
        NormalMismatch = 1u << 3u,
    };

    inline float DeviceDepthResidual(
        const float expectedPreviousDepth,
        const float historyDepth)
    {
        return std::abs(expectedPreviousDepth - historyDepth)
            / std::max({1.0f - expectedPreviousDepth, 1.0f - historyDepth, 1.0e-5f});
    }

    inline std::uint32_t Classify(
        const bool historyValid,
        const Uv historyUv,
        const std::uint32_t currentOwner,
        const std::uint32_t previousOwner,
        const float expectedPreviousDeviceDepth,
        const float historyDeviceDepth,
        const float normalDot,
        const float depthRelativeThreshold = 0.02f,
        const float normalDotThreshold = 0.85f)
    {
        if (!historyValid || !std::isfinite(historyUv.x) || !std::isfinite(historyUv.y)
            || historyUv.x < 0.0f || historyUv.x > 1.0f
            || historyUv.y < 0.0f || historyUv.y > 1.0f)
        {
            return InvalidHistoryOrUv;
        }
        std::uint32_t result = currentOwner != previousOwner ? OwnerMismatch : None;
        const float relativeDepth = DeviceDepthResidual(
            expectedPreviousDeviceDepth, historyDeviceDepth);
        if (relativeDepth > depthRelativeThreshold)
            result |= DepthMismatch;
        if (normalDot < normalDotThreshold)
            result |= NormalMismatch;
        return result;
    }
}
