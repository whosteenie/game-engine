#include "engine/rendering/Texture.h"

#include <glad/glad.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    std::vector<unsigned char> FlipRowsVertically(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount)
    {
        const int rowBytes = width * channelCount;
        std::vector<unsigned char> flipped(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height));
        for (int row = 0; row < height; ++row)
        {
            const int destinationRow = height - 1 - row;
            const std::size_t sourceOffset = static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
            const std::size_t destinationOffset = static_cast<std::size_t>(destinationRow) * static_cast<std::size_t>(rowBytes);
            std::copy(
                pixels + sourceOffset,
                pixels + sourceOffset + static_cast<std::size_t>(rowBytes),
                flipped.begin() + destinationOffset);
        }
        return flipped;
    }

    void ApplySamplerSettings(const TextureSamplerSettings& samplerSettings)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(samplerSettings.wrapS));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(samplerSettings.wrapT));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(samplerSettings.minFilter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(samplerSettings.magFilter));
    }

    bool ShouldGenerateMipmaps(const TextureSamplerSettings& samplerSettings)
    {
        return samplerSettings.minFilter == GL_LINEAR_MIPMAP_LINEAR ||
            samplerSettings.minFilter == GL_LINEAR_MIPMAP_NEAREST ||
            samplerSettings.minFilter == GL_NEAREST_MIPMAP_LINEAR ||
            samplerSettings.minFilter == GL_NEAREST_MIPMAP_NEAREST;
    }

    void UploadTexture2D(
        unsigned int& textureId,
        int width,
        int height,
        int channelCount,
        const unsigned char* pixels,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings,
        bool generateMipmaps)
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
        ApplySamplerSettings(samplerSettings);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
        if (generateMipmaps)
        {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

Texture::Texture(const char* path, TextureColorSpace colorSpace, bool flipVertically)
    : Texture(path, colorSpace, TextureSamplerSettings{}, flipVertically)
{
}

Texture::Texture(
    const char* path,
    TextureColorSpace colorSpace,
    const TextureSamplerSettings& samplerSettings,
    bool flipVertically)
{
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    unsigned char* pixels = stbi_load(path, &width, &height, &channelCount, 0);
    if (pixels == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load texture: ") + path);
    }

    UploadTexture2D(m_id, width, height, channelCount, pixels, colorSpace, samplerSettings, ShouldGenerateMipmaps(samplerSettings));
    stbi_image_free(pixels);
    m_valid = true;
}

std::shared_ptr<Texture> Texture::CreateFromPixels(
    const unsigned char* pixels,
    int width,
    int height,
    int channelCount,
    TextureColorSpace colorSpace,
    const TextureSamplerSettings& samplerSettings,
    bool flipVertically)
{
    auto texture = std::shared_ptr<Texture>(new Texture());
    const bool generateMipmaps = ShouldGenerateMipmaps(samplerSettings);

    std::vector<unsigned char> flippedPixels;
    const unsigned char* uploadPixels = pixels;
    if (flipVertically)
    {
        flippedPixels = FlipRowsVertically(pixels, width, height, channelCount);
        uploadPixels = flippedPixels.data();
    }

    UploadTexture2D(
        texture->m_id,
        width,
        height,
        channelCount,
        uploadPixels,
        colorSpace,
        samplerSettings,
        generateMipmaps);
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
