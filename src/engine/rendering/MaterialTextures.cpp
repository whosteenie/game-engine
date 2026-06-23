#include "engine/rendering/MaterialTextures.h"

#include "engine/rendering/Constants.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"

void ApplyWoodTableMaterialMaps(Material& material)
{
    TextureCache& cache = TextureCache::Get();

    material.SetAlbedoMap(cache.Load(EngineConstants::CubeAlbedoTexture, TextureColorSpace::SRGB), EngineConstants::CubeAlbedoTexture);
    material.SetNormalMap(cache.Load(EngineConstants::CubeNormalTexture, TextureColorSpace::Linear), EngineConstants::CubeNormalTexture);
    material.SetAoMap(cache.Load(EngineConstants::CubeAoTexture, TextureColorSpace::Linear), EngineConstants::CubeAoTexture);
    material.SetRoughnessMap(cache.Load(EngineConstants::CubeRoughnessTexture, TextureColorSpace::Linear), EngineConstants::CubeRoughnessTexture);
}

void ApplyConcreteFloorMaterialMaps(Material& material)
{
    TextureCache& cache = TextureCache::Get();

    material.SetAlbedoMap(cache.Load(EngineConstants::FloorAlbedoTexture, TextureColorSpace::SRGB), EngineConstants::FloorAlbedoTexture);
    material.SetNormalMap(cache.Load(EngineConstants::FloorNormalTexture, TextureColorSpace::Linear), EngineConstants::FloorNormalTexture);
    material.SetAoMap(cache.Load(EngineConstants::FloorAoTexture, TextureColorSpace::Linear), EngineConstants::FloorAoTexture);
    material.SetRoughnessMap(cache.Load(EngineConstants::FloorRoughnessTexture, TextureColorSpace::Linear), EngineConstants::FloorRoughnessTexture);
}
