#include "engine/assets/gltf/Detail.h"

#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/assets/TangentSpace.h"
#include "engine/assets/ProjectAssets.h"
#include "engine/rendering/resources/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/rendering/resources/TextureSamplerSettings.h"
#include "primitives/PrimitiveMeshUtils.h"
#include "engine/platform/diagnostics/RenderPathDiagnostics.h"

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace GltfDetail
{
    std::unique_ptr<Material> CreateMaterial(
        const tinygltf::Model& model,
        int materialIndex,
        const std::string& modelDirectory,
        const std::string& projectRoot,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache)
    {
        glm::vec3 albedo(0.8f);
        float roughness = 0.5f;
        float metallic = 0.0f;
        glm::vec3 emissive(0.0f);

        if (materialIndex >= 0)
        {
            const tinygltf::Material& gltfMaterial = model.materials[static_cast<std::size_t>(materialIndex)];
            const auto& pbr = gltfMaterial.pbrMetallicRoughness;
            albedo = glm::vec3(
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2]));
            roughness = static_cast<float>(pbr.roughnessFactor);
            metallic = static_cast<float>(pbr.metallicFactor);
            if (gltfMaterial.emissiveFactor.size() >= 3)
            {
                emissive = glm::vec3(
                    static_cast<float>(gltfMaterial.emissiveFactor[0]),
                    static_cast<float>(gltfMaterial.emissiveFactor[1]),
                    static_cast<float>(gltfMaterial.emissiveFactor[2]));
            }

            auto material = std::make_unique<Material>(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                albedo,
                roughness,
                metallic);
            material->SetDoubleSided(gltfMaterial.doubleSided);
            material->SetEmissive(emissive);

            if (pbr.baseColorTexture.index >= 0)
            {
                material->SetAlbedoTexCoordSet(pbr.baseColorTexture.texCoord);
                const std::string texturePath = GetGltfTextureFilePath(
                    model,
                    pbr.baseColorTexture.index,
                    modelDirectory);
                std::shared_ptr<Texture> texture = LoadGltfTexture(
                    model,
                    pbr.baseColorTexture.index,
                    modelDirectory,
                    TextureColorSpace::SRGB,
                    textureCache);
                if (texture != nullptr && texture->IsValid())
                {
                    material->SetAlbedoMap(texture, StoreTexturePath(projectRoot, texturePath));
                }
            }

            if (pbr.metallicRoughnessTexture.index >= 0)
            {
                const std::string texturePath = GetGltfTextureFilePath(
                    model,
                    pbr.metallicRoughnessTexture.index,
                    modelDirectory);
                std::shared_ptr<Texture> texture = LoadGltfTexture(
                    model,
                    pbr.metallicRoughnessTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    textureCache);
                if (texture != nullptr && texture->IsValid())
                {
                    material->SetMetallicRoughnessMap(
                        texture,
                        pbr.metallicRoughnessTexture.texCoord,
                        StoreTexturePath(projectRoot, texturePath));
                }
            }

            if (gltfMaterial.normalTexture.index >= 0)
            {
                material->SetNormalTexCoordSet(gltfMaterial.normalTexture.texCoord);
                const std::string texturePath = GetGltfTextureFilePath(
                    model,
                    gltfMaterial.normalTexture.index,
                    modelDirectory);
                std::shared_ptr<Texture> texture = LoadGltfTexture(
                    model,
                    gltfMaterial.normalTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    textureCache);
                if (texture != nullptr && texture->IsValid())
                {
                    material->SetNormalMap(texture, StoreTexturePath(projectRoot, texturePath));
                }
            }

            if (gltfMaterial.occlusionTexture.index >= 0)
            {
                material->SetAoTexCoordSet(gltfMaterial.occlusionTexture.texCoord);
                const std::string texturePath = GetGltfTextureFilePath(
                    model,
                    gltfMaterial.occlusionTexture.index,
                    modelDirectory);
                std::shared_ptr<Texture> texture = LoadGltfTexture(
                    model,
                    gltfMaterial.occlusionTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    textureCache);
                if (texture != nullptr && texture->IsValid())
                {
                    material->SetAoMap(texture, StoreTexturePath(projectRoot, texturePath));
                }
            }

            if (gltfMaterial.emissiveTexture.index >= 0)
            {
                material->SetEmissiveTexCoordSet(gltfMaterial.emissiveTexture.texCoord);
                const std::string texturePath = GetGltfTextureFilePath(
                    model,
                    gltfMaterial.emissiveTexture.index,
                    modelDirectory);
                std::shared_ptr<Texture> texture = LoadGltfTexture(
                    model,
                    gltfMaterial.emissiveTexture.index,
                    modelDirectory,
                    TextureColorSpace::SRGB,
                    textureCache);
                if (texture != nullptr && texture->IsValid())
                {
                    material->SetEmissiveMap(texture, StoreTexturePath(projectRoot, texturePath));
                }
            }

            return material;
        }

        return std::make_unique<Material>(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            albedo,
            roughness,
            metallic);
    }

}
