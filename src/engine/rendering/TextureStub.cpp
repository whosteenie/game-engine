#include "engine/rendering/Texture.h"

Texture::Texture(const char* path, TextureColorSpace colorSpace, bool flipVertically)
    : Texture(path, colorSpace, TextureSamplerSettings{}, flipVertically)
{
}

Texture::Texture(
    const char* path,
    TextureColorSpace /*colorSpace*/,
    const TextureSamplerSettings& /*samplerSettings*/,
    bool /*flipVertically*/)
{
    (void)path;
    m_valid = false;
    m_id = 0;
}

std::shared_ptr<Texture> Texture::CreateFromPixels(
    const unsigned char* /*pixels*/,
    int /*width*/,
    int /*height*/,
    int /*channelCount*/,
    TextureColorSpace /*colorSpace*/,
    const TextureSamplerSettings& /*samplerSettings*/,
    bool /*flipVertically*/)
{
    return std::shared_ptr<Texture>(new Texture());
}

Texture::~Texture() = default;

void Texture::Bind(unsigned int /*textureUnit*/) const
{
}

unsigned int Texture::GetId() const
{
    return m_id;
}

bool Texture::IsValid() const
{
    return m_valid;
}
