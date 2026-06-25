#pragma once

#include <d3d12.h>

namespace TexSampler
{
    constexpr unsigned int WrapRepeat = 1;
    constexpr unsigned int WrapClampToEdge = 2;
    constexpr unsigned int WrapMirroredRepeat = 3;

    constexpr unsigned int FilterNearest = 10;
    constexpr unsigned int FilterLinear = 11;
    constexpr unsigned int FilterNearestMipmapNearest = 12;
    constexpr unsigned int FilterNearestMipmapLinear = 13;
    constexpr unsigned int FilterLinearMipmapNearest = 14;
    constexpr unsigned int FilterLinearMipmapLinear = 15;
}

enum class TextureFilterMode
{
    Trilinear = 0,
    Bilinear = 1,
    Nearest = 2,
};

struct TextureSamplerSettings
{
    unsigned int wrapS = TexSampler::WrapRepeat;
    unsigned int wrapT = TexSampler::WrapRepeat;
    unsigned int minFilter = TexSampler::FilterLinearMipmapLinear;
    unsigned int magFilter = TexSampler::FilterLinear;
};

inline D3D12_FILTER TextureFilterModeToD3D12Filter(TextureFilterMode mode)
{
    switch (mode)
    {
    case TextureFilterMode::Bilinear:
        return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case TextureFilterMode::Nearest:
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case TextureFilterMode::Trilinear:
    default:
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
}
