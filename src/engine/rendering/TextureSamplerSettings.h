#pragma once

#if defined(GAME_ENGINE_D3D12)

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

struct TextureSamplerSettings
{
    unsigned int wrapS = TexSampler::WrapRepeat;
    unsigned int wrapT = TexSampler::WrapRepeat;
    unsigned int minFilter = TexSampler::FilterLinearMipmapLinear;
    unsigned int magFilter = TexSampler::FilterLinear;
};

#else

#include <glad/glad.h>

struct TextureSamplerSettings
{
    unsigned int wrapS = GL_REPEAT;
    unsigned int wrapT = GL_REPEAT;
    unsigned int minFilter = GL_LINEAR_MIPMAP_LINEAR;
    unsigned int magFilter = GL_LINEAR;
};

#endif
