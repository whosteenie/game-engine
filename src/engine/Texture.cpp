#include "engine/Texture.h"

#include <glad/glad.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <stdexcept>
#include <string>

namespace
{
    void UploadTexture2D(
        unsigned int& textureId,
        int width,
        int height,
        int channelCount,
        const unsigned char* pixels,
        TextureColorSpace colorSpace)
    {
        GLenum format = GL_RGB;
        if (channelCount == 1)
        {
            format = GL_RED;
        }
        else if (channelCount == 4)
        {
            format = GL_RGBA;
        }

        const GLenum internalFormat = colorSpace == TextureColorSpace::SRGB
            ? (channelCount == 4 ? GL_SRGB8_ALPHA8 : GL_SRGB8)
            : (channelCount == 4 ? GL_RGBA8 : GL_RGB8);

        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

Texture::Texture(const char* path, TextureColorSpace colorSpace)
{
    stbi_set_flip_vertically_on_load(true);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    unsigned char* pixels = stbi_load(path, &width, &height, &channelCount, 0);
    if (pixels == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load texture: ") + path);
    }

    UploadTexture2D(m_id, width, height, channelCount, pixels, colorSpace);
    stbi_image_free(pixels);
    m_valid = true;
}

std::shared_ptr<Texture> Texture::CreateFromPixels(
    const unsigned char* pixels,
    int width,
    int height,
    int channelCount,
    TextureColorSpace colorSpace)
{
    auto texture = std::shared_ptr<Texture>(new Texture());
    UploadTexture2D(texture->m_id, width, height, channelCount, pixels, colorSpace);
    texture->m_valid = true;
    return texture;
}

Texture::~Texture()
{
    if (m_id != 0)
    {
        glDeleteTextures(1, &m_id);
    }
}

void Texture::Bind(unsigned int textureUnit) const
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

unsigned int Texture::GetId() const
{
    return m_id;
}

bool Texture::IsValid() const
{
    return m_valid;
}
