#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace nrd
{
enum class Format : uint32_t;
}

namespace NrdCommon
{
    std::uint32_t NrdFormatToDxgi(nrd::Format format);

    void TransitionTracked(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        std::uint32_t& state,
        std::uint32_t newState);
}
