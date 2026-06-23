#pragma once

#include "engine/rendering/TextureSamplerSettings.h"

#include <memory>

enum class TextureColorSpace
{
    Linear = 0,
    SRGB = 1
};

class Texture
{
public:
    Texture(const char* path, TextureColorSpace colorSpace, bool flipVertically = true);
    Texture(
        const char* path,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings,
        bool flipVertically = true);
    static std::shared_ptr<Texture> CreateFromPixels(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings = TextureSamplerSettings{},
        bool flipVertically = false);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void Bind(unsigned int textureUnit) const;
    unsigned int GetId() const;
    bool IsValid() const;

private:
    Texture() = default;

    unsigned int m_id = 0;
    bool m_valid = false;
};
