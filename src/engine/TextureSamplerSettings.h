#pragma once

struct TextureSamplerSettings
{
    unsigned int wrapS = 0x2901; // GL_REPEAT
    unsigned int wrapT = 0x2901; // GL_REPEAT
    unsigned int minFilter = 0x2703; // GL_LINEAR_MIPMAP_LINEAR
    unsigned int magFilter = 0x2601; // GL_LINEAR
};
