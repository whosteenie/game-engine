#include "engine/assets/TextureCache.h"

#include "engine/platform/system/ExceptionMessage.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/rendering/resources/TextureSamplerSettings.h"

#include <stdexcept>
#include <string>

namespace
{
    std::string MakeCacheKey(
        const char* path,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings,
        bool flipVertically)
    {
        return std::string(path) + "|" +
            (colorSpace == TextureColorSpace::SRGB ? "srgb" : "linear") + "|" +
            std::to_string(samplerSettings.wrapS) + "|" +
            std::to_string(samplerSettings.wrapT) + "|" +
            std::to_string(samplerSettings.minFilter) + "|" +
            std::to_string(samplerSettings.magFilter) + "|" +
            (flipVertically ? "flip" : "noflip");
    }
}

TextureCache& TextureCache::Get()
{
    static TextureCache cache;
    return cache;
}

std::shared_ptr<Texture> TextureCache::Load(
    const char* path,
    TextureColorSpace colorSpace,
    const TextureSamplerSettings& samplerSettings,
    bool flipVertically)
{
    const std::string cacheKey = MakeCacheKey(path, colorSpace, samplerSettings, flipVertically);

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
        ProjectLoadBenchmark::ScopedPhase textureLoadPhase("renderer.texture_cache_miss_load");
        texture = std::make_shared<Texture>(path, colorSpace, samplerSettings, flipVertically);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("TextureCache failed to load '") + path + "': " + SafeExceptionMessage(exception));
    }

    m_textures[cacheKey] = texture;
    return texture;
}

void TextureCache::Clear()
{
    m_textures.clear();
}
