#pragma once

#include <glad/glad.h>

struct TextureSamplerSettings
{
    unsigned int wrapS = GL_REPEAT;
    unsigned int wrapT = GL_REPEAT;
    unsigned int minFilter = GL_LINEAR_MIPMAP_LINEAR;
    unsigned int magFilter = GL_LINEAR;
};
