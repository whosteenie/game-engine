#pragma once

#include <cstdint>

// Offscreen color/depth targets used by the post-process chain (HK-C0).
struct PostProcessTarget
{
    void* resource = nullptr;
    void* allocation = nullptr;
    std::uint32_t srvIndex = UINT32_MAX;
    std::uintptr_t srvCpuHandle = 0;
    std::uint32_t rtvIndex = UINT32_MAX;
    int width = 0;
    int height = 0;
    int format = 0;
    // Tracked D3D12 resource state; 0 means pixel-shader-resource (set in CreateInternalTarget).
    mutable std::uint32_t resourceState = 0;
};

struct PostProcessDepthTarget
{
    void* resource = nullptr;
    void* allocation = nullptr;
    std::uint32_t dsvIndex = UINT32_MAX;
    std::uint32_t srvIndex = UINT32_MAX;
    std::uintptr_t srvCpuHandle = 0;
    int width = 0;
    int height = 0;
    mutable std::uint32_t resourceState = 0;
};
