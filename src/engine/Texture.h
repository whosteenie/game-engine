#pragma once

enum class TextureColorSpace
{
    Linear = 0,
    SRGB = 1
};

class Texture
{
public:
    Texture(const char* path, TextureColorSpace colorSpace);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void Bind(unsigned int textureUnit) const;
    unsigned int GetId() const;
    bool IsValid() const;

private:
    unsigned int m_id = 0;
    bool m_valid = false;
};
