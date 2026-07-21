#include "engine/rendering/resources/MaterialTextures.h"

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Texture.h"
#include "engine/assets/TextureCache.h"

#include <functional>

namespace
{
    void TryAssignMap(
        Material& material,
        const char* label,
        const char* path,
        TextureColorSpace colorSpace,
        const std::function<void(std::shared_ptr<Texture>, const char*)>& assign)
    {
        try
        {
            TextureCache& cache = TextureCache::Get();
            assign(cache.Load(path, colorSpace), path);
        }
        catch (const std::exception& exception)
        {
            EngineLog::LogFailure(
                "material",
                "TryAssignMap",
                FormatExceptionContext(
                    (std::string("Failed to load ") + label + " map '" + path + "'").c_str(),
                    exception));
        }
    }
}

void ApplyWoodTableMaterialMaps(Material& material)
{
    TryAssignMap(
        material,
        "albedo",
        EngineConstants::CubeAlbedoTexture,
        TextureColorSpace::SRGB,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetAlbedoMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "normal",
        EngineConstants::CubeNormalTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetNormalMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "ao",
        EngineConstants::CubeAoTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetAoMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "roughness",
        EngineConstants::CubeRoughnessTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetRoughnessMap(std::move(texture), path);
        });
}

void AssignWoodTableMaterialMapPaths(Material& material)
{
    material.SetAlbedoMap(nullptr, EngineConstants::CubeAlbedoTexture);
    material.SetNormalMap(nullptr, EngineConstants::CubeNormalTexture);
    material.SetAoMap(nullptr, EngineConstants::CubeAoTexture);
    material.SetRoughnessMap(nullptr, EngineConstants::CubeRoughnessTexture);
}

void AssignConcreteFloorMaterialMapPaths(Material& material)
{
    material.SetAlbedoMap(nullptr, EngineConstants::FloorAlbedoTexture);
    material.SetNormalMap(nullptr, EngineConstants::FloorNormalTexture);
    material.SetAoMap(nullptr, EngineConstants::FloorAoTexture);
    material.SetRoughnessMap(nullptr, EngineConstants::FloorRoughnessTexture);
}

void ApplyConcreteFloorMaterialMaps(Material& material)
{
    TryAssignMap(
        material,
        "albedo",
        EngineConstants::FloorAlbedoTexture,
        TextureColorSpace::SRGB,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetAlbedoMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "normal",
        EngineConstants::FloorNormalTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetNormalMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "ao",
        EngineConstants::FloorAoTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetAoMap(std::move(texture), path);
        });
    TryAssignMap(
        material,
        "roughness",
        EngineConstants::FloorRoughnessTexture,
        TextureColorSpace::Linear,
        [&](std::shared_ptr<Texture> texture, const char* path) {
            material.SetRoughnessMap(std::move(texture), path);
        });
}
