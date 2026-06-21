#include "engine/TextureCache.h"

#include <stdexcept>
#include <string>

TextureCache& TextureCache::Get()
{
    static TextureCache cache;
    return cache;
}

std::shared_ptr<Texture> TextureCache::Load(const char* path, TextureColorSpace colorSpace)
{
    const std::string cacheKey = std::string(path) + (colorSpace == TextureColorSpace::SRGB ? "|srgb" : "|linear");

    const auto existing = m_textures.find(cacheKey);
    if (existing != m_textures.end())
    {
        if (std::shared_ptr<Texture> texture = existing->second.lock())
        {
            return texture;
        }
    }

    std::shared_ptr<Texture> texture;
    try
    {
        texture = std::make_shared<Texture>(path, colorSpace);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(std::string("TextureCache failed to load '") + path + "': " + exception.what());
    }

    m_textures[cacheKey] = texture;
    return texture;
}
