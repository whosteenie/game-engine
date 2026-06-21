#include "engine/MaterialTextures.h"

#include "engine/Constants.h"
#include "engine/Material.h"
#include "engine/Texture.h"
#include "engine/TextureCache.h"

void ApplyWoodTableMaterialMaps(Material& material)
{
    TextureCache& cache = TextureCache::Get();

    material.SetAlbedoMap(cache.Load(EngineConstants::CubeAlbedoTexture, TextureColorSpace::SRGB));
    material.SetNormalMap(cache.Load(EngineConstants::CubeNormalTexture, TextureColorSpace::Linear));
    material.SetAoMap(cache.Load(EngineConstants::CubeAoTexture, TextureColorSpace::Linear));
    material.SetRoughnessMap(cache.Load(EngineConstants::CubeRoughnessTexture, TextureColorSpace::Linear));
}

void ApplyConcreteFloorMaterialMaps(Material& material)
{
    TextureCache& cache = TextureCache::Get();

    material.SetAlbedoMap(cache.Load(EngineConstants::FloorAlbedoTexture, TextureColorSpace::SRGB));
    material.SetNormalMap(cache.Load(EngineConstants::FloorNormalTexture, TextureColorSpace::Linear));
    material.SetAoMap(cache.Load(EngineConstants::FloorAoTexture, TextureColorSpace::Linear));
    material.SetRoughnessMap(cache.Load(EngineConstants::FloorRoughnessTexture, TextureColorSpace::Linear));
}
