#pragma once

#include "engine/Texture.h"

#include <memory>
#include <string>
#include <unordered_map>

class TextureCache
{
public:
    static TextureCache& Get();

    std::shared_ptr<Texture> Load(
        const char* path,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings = TextureSamplerSettings{},
        bool flipVertically = true);

private:
    TextureCache() = default;

    std::unordered_map<std::string, std::weak_ptr<Texture>> m_textures;
};
