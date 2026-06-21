#pragma once

#include <memory>

enum class TextureColorSpace
{
    Linear = 0,
    SRGB = 1
};

class Texture
{
public:
    Texture(const char* path, TextureColorSpace colorSpace);
    static std::shared_ptr<Texture> CreateFromPixels(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount,
        TextureColorSpace colorSpace);
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
