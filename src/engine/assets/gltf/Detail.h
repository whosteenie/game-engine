#pragma once

#include "engine/assets/ModelImporter.h"

// tinygltf must be included before headers that pull nlohmann json_fwd (for example Material.h).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_JSON
#include <nlohmann/json.hpp>
#ifdef GLTF_DETAIL_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#endif
#include <tiny_gltf.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Material;
class Mesh;
class Texture;
enum class TextureColorSpace;

namespace GltfDetail
{
    bool LoadImageData(
        tinygltf::Image* image,
        int imageIndex,
        std::string* error,
        std::string* warning,
        int requestedWidth,
        int requestedHeight,
        const unsigned char* bytes,
        int size,
        void* userData);

    std::string GetFileExtensionLower(const std::string& path);
    std::string GetModelDirectory(const std::string& path);
    void ExtractEmbeddedImages(
        const tinygltf::Model& model,
        const std::string& modelDirectory,
        const ModelOperationProgressFn& onProgress = {});

    void ResetTextureStats();
    int GetTextureLoadFailureCount();
    int GetTextureCacheHitCount();

    std::string GetGltfTextureFilePath(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory);
    std::string StoreTexturePath(const std::string& projectRoot, const std::string& absolutePath);
    std::shared_ptr<Texture> LoadGltfTexture(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory,
        TextureColorSpace colorSpace,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache);

    std::unique_ptr<Material> CreateMaterial(
        const tinygltf::Model& model,
        int materialIndex,
        const std::string& modelDirectory,
        const std::string& projectRoot,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache);

    bool BuildMesh(
        const tinygltf::Model& model,
        const tinygltf::Primitive& primitive,
        std::unique_ptr<Mesh>& outMesh,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax,
        bool benchmarkProjectGeometryLoad);

    Transform TransformFromMatrix(const glm::mat4& matrix);
    glm::mat4 GetLocalNodeMatrix(const tinygltf::Node& node);

    void VisitNode(
        const tinygltf::Model& model,
        int nodeIndex,
        int parentNodeIndex,
        const std::string& modelDirectory,
        const std::string& projectRoot,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache,
        std::vector<ImportedSceneNode>& nodes,
        int& nameCounter,
        int totalNodes,
        int& processedNodes,
        float nodeProgressStart,
        ModelLoadMode loadMode,
        const ModelOperationProgressFn& onProgress);
}
