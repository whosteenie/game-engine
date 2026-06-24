#include "engine/assets/ModelImporter.h"

#include "engine/rendering/Constants.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/assets/TangentSpace.h"
#include "engine/assets/ProjectAssets.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/rendering/TextureSamplerSettings.h"
#include "primitives/PrimitiveMeshUtils.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include "engine/platform/RenderPathDiagnostics.h"

#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define GLM_ENABLE_EXPERIMENTAL
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

namespace
{
    bool LoadImageData(
        tinygltf::Image* image,
        const int /*imageIndex*/,
        std::string* err,
        std::string* /*warn*/,
        int /*requestedWidth*/,
        int /*requestedHeight*/,
        const unsigned char* bytes,
        int size,
        void* /*userData*/)
    {
        int width = 0;
        int height = 0;
        int componentCount = 0;
        unsigned char* pixels = stbi_load_from_memory(bytes, size, &width, &height, &componentCount, 0);
        if (pixels == nullptr)
        {
            if (err != nullptr)
            {
                *err += "Failed to decode glTF image data.";
            }
            return false;
        }

        image->width = width;
        image->height = height;
        image->component = componentCount;
        image->image.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(componentCount));
        stbi_image_free(pixels);
        return true;
    }

    std::string GetFileExtensionLower(const std::string& path)
    {
        const std::filesystem::path filePath(path);
        std::string extension = filePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return extension;
    }

    std::string GetModelDirectory(const std::string& path)
    {
        return std::filesystem::path(path).parent_path().string();
    }

    std::string JoinPath(const std::string& directory, const std::string& relativePath)
    {
        return (std::filesystem::path(directory) / relativePath).string();
    }

    std::string GetExtractedEmbeddedImagePath(const std::string& modelDirectory, int imageIndex)
    {
        return JoinPath(modelDirectory, "textures/image_" + std::to_string(imageIndex) + ".png");
    }

    void ExtractEmbeddedGltfImages(
        const tinygltf::Model& model,
        const std::string& modelDirectory,
        const ModelOperationProgressFn& onProgress = {})
    {
        if (modelDirectory.empty() || model.images.empty())
        {
            if (onProgress)
            {
                onProgress(1.0f, {});
            }
            return;
        }

        namespace fs = std::filesystem;
        const fs::path texturesDirectory = fs::path(modelDirectory) / "textures";
        std::error_code error;
        fs::create_directories(texturesDirectory, error);

        std::vector<std::size_t> embeddedImageIndices;
        embeddedImageIndices.reserve(model.images.size());
        for (std::size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex)
        {
            const tinygltf::Image& image = model.images[imageIndex];
            if (!image.uri.empty() && image.uri.rfind("data:", 0) != 0)
            {
                continue;
            }

            if (image.image.empty() || image.width <= 0 || image.height <= 0 || image.component < 1)
            {
                continue;
            }

            const fs::path outputPath = texturesDirectory / ("image_" + std::to_string(imageIndex) + ".png");
            if (fs::exists(outputPath))
            {
                continue;
            }

            embeddedImageIndices.push_back(imageIndex);
        }

        const std::size_t imageCount = embeddedImageIndices.size();
        if (imageCount == 0)
        {
            if (onProgress)
            {
                onProgress(1.0f, {});
            }
            return;
        }

        for (std::size_t writeIndex = 0; writeIndex < imageCount; ++writeIndex)
        {
            const std::size_t imageIndex = embeddedImageIndices[writeIndex];
            const tinygltf::Image& image = model.images[imageIndex];
            const fs::path outputPath = texturesDirectory / ("image_" + std::to_string(imageIndex) + ".png");

            if (onProgress)
            {
                const std::string detail =
                    "texture " + std::to_string(writeIndex + 1) + "/" + std::to_string(imageCount);
                onProgress(static_cast<float>(writeIndex) / static_cast<float>(imageCount), detail);
            }

            const int stride = image.width * image.component;
            stbi_write_png(
                outputPath.string().c_str(),
                image.width,
                image.height,
                image.component,
                image.image.data(),
                stride);
        }

        if (onProgress)
        {
            onProgress(1.0f, {});
        }
    }

    template<typename T>
    const T* GetAccessorData(
        const tinygltf::Model& model,
        int accessorIndex,
        int& componentCount,
        int& accessorCount)
    {
        if (accessorIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessorIndex)];
        const tinygltf::BufferView& bufferView = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(bufferView.buffer)];

        componentCount = tinygltf::GetNumComponentsInType(accessor.type);
        accessorCount = static_cast<int>(accessor.count);

        return reinterpret_cast<const T*>(
            &buffer.data[bufferView.byteOffset + accessor.byteOffset]);
    }

    glm::vec3 ReadVec3(const float* data, int index)
    {
        const int offset = index * 3;
        return glm::vec3(data[offset], data[offset + 1], data[offset + 2]);
    }

    glm::vec2 ReadVec2(const float* data, int index)
    {
        const int offset = index * 2;
        return glm::vec2(data[offset], data[offset + 1]);
    }

    void ExpandBounds(glm::vec3& boundsMin, glm::vec3& boundsMax, const glm::vec3& point)
    {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }

    unsigned int ToSamplerWrap(int wrap)
    {
        switch (wrap)
        {
        case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
            return TexSampler::WrapClampToEdge;
        case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
            return TexSampler::WrapMirroredRepeat;
        case TINYGLTF_TEXTURE_WRAP_REPEAT:
        default:
            return TexSampler::WrapRepeat;
        }
    }

    unsigned int ToSamplerMinFilter(int filter)
    {
        switch (filter)
        {
        case TINYGLTF_TEXTURE_FILTER_NEAREST:
            return TexSampler::FilterNearest;
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
            return TexSampler::FilterNearestMipmapNearest;
        case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
            return TexSampler::FilterNearestMipmapLinear;
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
            return TexSampler::FilterLinearMipmapNearest;
        case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
            return TexSampler::FilterLinearMipmapLinear;
        case TINYGLTF_TEXTURE_FILTER_LINEAR:
        default:
            return TexSampler::FilterLinear;
        }
    }

    unsigned int ToSamplerMagFilter(int filter)
    {
        switch (filter)
        {
        case TINYGLTF_TEXTURE_FILTER_NEAREST:
            return TexSampler::FilterNearest;
        case TINYGLTF_TEXTURE_FILTER_LINEAR:
        default:
            return TexSampler::FilterLinear;
        }
    }

    TextureSamplerSettings GetGltfSamplerSettings(const tinygltf::Model& model, int textureIndex)
    {
        TextureSamplerSettings settings;

        if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
        {
            return settings;
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        if (texture.sampler < 0 || texture.sampler >= static_cast<int>(model.samplers.size()))
        {
            return settings;
        }

        const tinygltf::Sampler& sampler = model.samplers[static_cast<std::size_t>(texture.sampler)];
        settings.wrapS = ToSamplerWrap(sampler.wrapS);
        settings.wrapT = ToSamplerWrap(sampler.wrapT);
        settings.minFilter = ToSamplerMinFilter(sampler.minFilter);
        settings.magFilter = ToSamplerMagFilter(sampler.magFilter);

        return settings;
    }

    std::string MakeTextureCacheKey(
        int imageIndex,
        const TextureSamplerSettings& samplerSettings,
        TextureColorSpace colorSpace,
        const char* prefix = "")
    {
        return std::string(prefix) +
            std::to_string(imageIndex) + ":" +
            std::to_string(samplerSettings.wrapS) + ":" +
            std::to_string(samplerSettings.wrapT) + ":" +
            std::to_string(samplerSettings.minFilter) + ":" +
            std::to_string(samplerSettings.magFilter) + ":" +
            (colorSpace == TextureColorSpace::SRGB ? "s" : "l");
    }

    int g_importTextureFailures = 0;
    int g_importTexturesCached = 0;

    std::shared_ptr<Texture> LoadGltfImageTexture(
        const tinygltf::Model& model,
        int imageIndex,
        const std::string& modelDirectory,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache,
        const char* cachePrefix = "")
    {
        if (imageIndex < 0)
        {
            return nullptr;
        }

        const std::string cacheKey = MakeTextureCacheKey(imageIndex, samplerSettings, colorSpace, cachePrefix);
        const auto cachedTexture = textureCache.find(cacheKey);
        if (cachedTexture != textureCache.end())
        {
            return cachedTexture->second;
        }

        const tinygltf::Image& image = model.images[static_cast<std::size_t>(imageIndex)];
        std::shared_ptr<Texture> texture;

        if (!image.uri.empty() && image.uri.rfind("data:", 0) != 0)
        {
            const std::string texturePath = JoinPath(modelDirectory, image.uri);
            try
            {
                texture = TextureCache::Get().Load(
                    texturePath.c_str(),
                    colorSpace,
                    samplerSettings,
                    true);
            }
            catch (const std::exception& exception)
            {
                ++g_importTextureFailures;
                RenderPathDiagnostics::LogImportTextureFailure(
                    imageIndex,
                    std::string("file \"") + texturePath + "\" " + exception.what());
                texture = nullptr;
            }
        }
        else if (!image.image.empty() && image.width > 0 && image.height > 0)
        {
            try
            {
                texture = Texture::CreateFromPixels(
                    image.image.data(),
                    image.width,
                    image.height,
                    image.component,
                    colorSpace,
                    samplerSettings,
                    true);
            }
            catch (const std::exception& exception)
            {
                ++g_importTextureFailures;
                RenderPathDiagnostics::LogImportTextureFailure(
                    imageIndex,
                    std::string("embedded ") + std::to_string(image.width) + "x" +
                        std::to_string(image.height) + " " + exception.what());
                texture = nullptr;
            }
        }
        else if (imageIndex >= 0)
        {
            ++g_importTextureFailures;
            RenderPathDiagnostics::LogImportTextureFailure(
                imageIndex,
                "no file path and no embedded pixels");
        }

        if (texture != nullptr && texture->IsValid())
        {
            const bool inserted = textureCache.emplace(cacheKey, texture).second;
            if (inserted)
            {
                ++g_importTexturesCached;
            }
        }
        else if (texture != nullptr)
        {
            ++g_importTextureFailures;
            RenderPathDiagnostics::LogImportTextureFailure(imageIndex, "texture object is invalid after upload");
        }

        return texture;
    }

    std::string GetGltfImageFilePath(
        const tinygltf::Model& model,
        int imageIndex,
        const std::string& modelDirectory)
    {
        if (imageIndex < 0)
        {
            return {};
        }

        const tinygltf::Image& image = model.images[static_cast<std::size_t>(imageIndex)];
        if (!image.uri.empty() && image.uri.rfind("data:", 0) != 0)
        {
            return JoinPath(modelDirectory, image.uri);
        }

        if (!image.image.empty() && image.width > 0 && image.height > 0)
        {
            return GetExtractedEmbeddedImagePath(modelDirectory, imageIndex);
        }

        return {};
    }

    std::string GetGltfTextureFilePath(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory)
    {
        if (textureIndex < 0)
        {
            return {};
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        return GetGltfImageFilePath(model, texture.source, modelDirectory);
    }

    std::string StoreTexturePath(const std::string& projectRoot, const std::string& absolutePath)
    {
        if (absolutePath.empty())
        {
            return {};
        }

        if (projectRoot.empty())
        {
            return absolutePath;
        }

        return MakeProjectRelativePath(projectRoot, absolutePath);
    }

    std::shared_ptr<Texture> LoadGltfTexture(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory,
        TextureColorSpace colorSpace,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache)
    {
        if (textureIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        const TextureSamplerSettings samplerSettings = GetGltfSamplerSettings(model, textureIndex);
        return LoadGltfImageTexture(
            model,
            texture.source,
            modelDirectory,
            colorSpace,
            samplerSettings,
            textureCache);
    }

    std::unique_ptr<Material> CreateMaterialFromGltf(
        const tinygltf::Model& model,
        int materialIndex,
        const std::string& modelDirectory,
        const std::string& projectRoot,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache)
    {
        glm::vec3 albedo(0.8f);
        float roughness = 0.5f;
        float metallic = 0.0f;

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

            auto material = std::make_unique<Material>(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                albedo,
                roughness,
                metallic);
            material->SetDoubleSided(gltfMaterial.doubleSided);

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

            return material;
        }

        return std::make_unique<Material>(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            albedo,
            roughness,
            metallic);
    }

    Transform TransformFromMatrix(const glm::mat4& matrix)
    {
        Transform transform;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(matrix, transform.scale, transform.rotation, transform.position, skew, perspective);
        return transform;
    }

    glm::mat4 GetLocalNodeMatrix(const tinygltf::Node& node)
    {
        if (!node.matrix.empty())
        {
            glm::mat4 matrix(1.0f);
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    matrix[column][row] = static_cast<float>(node.matrix[static_cast<std::size_t>(column * 4 + row)]);
                }
            }
            return matrix;
        }

        glm::mat4 matrix(1.0f);
        if (!node.translation.empty())
        {
            matrix = glm::translate(
                matrix,
                glm::vec3(
                    static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2])));
        }

        if (!node.rotation.empty())
        {
            const glm::quat rotation(
                static_cast<float>(node.rotation[3]),
                static_cast<float>(node.rotation[0]),
                static_cast<float>(node.rotation[1]),
                static_cast<float>(node.rotation[2]));
            matrix *= glm::mat4_cast(rotation);
        }

        if (!node.scale.empty())
        {
            matrix = glm::scale(
                matrix,
                glm::vec3(
                    static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]),
                    static_cast<float>(node.scale[2])));
        }

        return matrix;
    }

    bool BuildMeshFromPrimitive(
        const tinygltf::Model& model,
        const tinygltf::Primitive& primitive,
        std::unique_ptr<Mesh>& outMesh,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax)
    {
        if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1)
        {
            return false;
        }

        const auto positionIt = primitive.attributes.find("POSITION");
        if (positionIt == primitive.attributes.end())
        {
            return false;
        }

        int positionComponentCount = 0;
        int positionCount = 0;
        const float* positions = GetAccessorData<float>(model, positionIt->second, positionComponentCount, positionCount);
        if (positions == nullptr || positionComponentCount < 3 || positionCount <= 0)
        {
            return false;
        }

        int normalComponentCount = 0;
        int normalCount = 0;
        const float* normals = nullptr;
        const auto normalIt = primitive.attributes.find("NORMAL");
        if (normalIt != primitive.attributes.end())
        {
            normals = GetAccessorData<float>(model, normalIt->second, normalComponentCount, normalCount);
        }

        int texCoordComponentCount = 0;
        int texCoordCount = 0;
        const float* texCoords = nullptr;
        const auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
        if (texCoordIt != primitive.attributes.end())
        {
            texCoords = GetAccessorData<float>(model, texCoordIt->second, texCoordComponentCount, texCoordCount);
        }

        int texCoord1ComponentCount = 0;
        int texCoord1Count = 0;
        const float* texCoords1 = nullptr;
        const auto texCoord1It = primitive.attributes.find("TEXCOORD_1");
        if (texCoord1It != primitive.attributes.end())
        {
            texCoords1 = GetAccessorData<float>(model, texCoord1It->second, texCoord1ComponentCount, texCoord1Count);
        }

        int tangentComponentCount = 0;
        int tangentCount = 0;
        const float* tangents = nullptr;
        const auto tangentIt = primitive.attributes.find("TANGENT");
        if (tangentIt != primitive.attributes.end())
        {
            tangents = GetAccessorData<float>(model, tangentIt->second, tangentComponentCount, tangentCount);
        }

        std::vector<unsigned int> indices;
        if (primitive.indices >= 0)
        {
            int indexComponentCount = 0;
            int indexCount = 0;
            const tinygltf::Accessor& indexAccessor = model.accessors[static_cast<std::size_t>(primitive.indices)];
            const tinygltf::BufferView& indexBufferView = model.bufferViews[static_cast<std::size_t>(indexAccessor.bufferView)];
            const tinygltf::Buffer& indexBuffer = model.buffers[static_cast<std::size_t>(indexBufferView.buffer)];
            const unsigned char* indexData =
                &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];
            indexComponentCount = tinygltf::GetNumComponentsInType(indexAccessor.type);
            indexCount = static_cast<int>(indexAccessor.count);

            indices.reserve(static_cast<std::size_t>(indexCount));
            for (int index = 0; index < indexCount; ++index)
            {
                unsigned int vertexIndex = 0;
                const std::size_t byteOffset = static_cast<std::size_t>(index) * static_cast<std::size_t>(indexAccessor.ByteStride(indexBufferView));
                switch (indexAccessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    vertexIndex = indexData[byteOffset];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    vertexIndex = *reinterpret_cast<const unsigned short*>(&indexData[byteOffset]);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    vertexIndex = *reinterpret_cast<const unsigned int*>(&indexData[byteOffset]);
                    break;
                default:
                    return false;
                }

                indices.push_back(vertexIndex);
            }
        }
        else
        {
            indices.reserve(static_cast<std::size_t>(positionCount));
            for (int index = 0; index < positionCount; ++index)
            {
                indices.push_back(static_cast<unsigned int>(index));
            }
        }

        boundsMin = glm::vec3(std::numeric_limits<float>::max());
        boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

        std::vector<float> vertices;
        vertices.reserve(static_cast<std::size_t>(positionCount) * Mesh::TexturedVertexFloatCount);

        for (int vertexIndex = 0; vertexIndex < positionCount; ++vertexIndex)
        {
            const glm::vec3 position = ReadVec3(positions, vertexIndex);
            glm::vec3 normal(0.0f, 1.0f, 0.0f);
            if (normals != nullptr && vertexIndex < normalCount)
            {
                normal = glm::normalize(ReadVec3(normals, vertexIndex));
            }

            glm::vec2 uv0(0.0f);
            if (texCoords != nullptr && vertexIndex < texCoordCount)
            {
                uv0 = ReadVec2(texCoords, vertexIndex);
            }

            glm::vec2 uv1(0.0f);
            if (texCoords1 != nullptr && vertexIndex < texCoord1Count)
            {
                uv1 = ReadVec2(texCoords1, vertexIndex);
            }

            glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            if (tangents != nullptr && vertexIndex < tangentCount && tangentComponentCount >= 3)
            {
                const int tangentOffset = vertexIndex * tangentComponentCount;
                const float handedness = tangentComponentCount >= 4 ? tangents[tangentOffset + 3] : 1.0f;
                const glm::vec3 tangentDirection = glm::normalize(glm::vec3(
                    tangents[tangentOffset],
                    tangents[tangentOffset + 1],
                    tangents[tangentOffset + 2]));
                tangent = glm::vec4(tangentDirection, handedness);
            }

            PrimitiveMesh::PushVertex(vertices, position, normal, uv0, tangent, uv1, texCoords1 != nullptr);
            ExpandBounds(boundsMin, boundsMax, position);
        }

        if (tangents == nullptr)
        {
            TangentSpace::GenerateMikkTSpaceTangents(vertices, indices);
        }

        for (std::size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex += Mesh::TexturedVertexFloatCount)
        {
            const glm::vec4 tangent(
                vertices[vertexIndex + 10],
                vertices[vertexIndex + 11],
                vertices[vertexIndex + 12],
                vertices[vertexIndex + 13]);
            const glm::vec4 finalized = TangentSpace::FinalizeImportTangent(tangent);
            vertices[vertexIndex + 10] = finalized.x;
            vertices[vertexIndex + 11] = finalized.y;
            vertices[vertexIndex + 12] = finalized.z;
            vertices[vertexIndex + 13] = finalized.w;
        }

        outMesh = PrimitiveMesh::BuildMesh(vertices, indices);
        return outMesh != nullptr;
    }

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
        const ModelOperationProgressFn& onProgress)
    {
        const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];

        ImportedSceneNode nodeObject;
        nodeObject.parentIndex = parentNodeIndex;
        nodeObject.transform = TransformFromMatrix(GetLocalNodeMatrix(node));
        if (!node.name.empty())
        {
            nodeObject.name = node.name;
        }
        else
        {
            nodeObject.name = "Node " + std::to_string(nameCounter++);
        }

        const int nodeObjectIndex = static_cast<int>(nodes.size());
        nodes.push_back(std::move(nodeObject));

        if (node.mesh >= 0)
        {
            const tinygltf::Mesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
            const bool singlePrimitive = mesh.primitives.size() == 1;

            for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
            {
                const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
                std::unique_ptr<Mesh> meshData;
                glm::vec3 boundsMin;
                glm::vec3 boundsMax;
                if (!BuildMeshFromPrimitive(model, primitive, meshData, boundsMin, boundsMax))
                {
                    continue;
                }

                std::unique_ptr<Material> material;
                if (loadMode == ModelLoadMode::Full)
                {
                    material = CreateMaterialFromGltf(
                        model,
                        primitive.material,
                        modelDirectory,
                        projectRoot,
                        textureCache);
                }

                if (singlePrimitive)
                {
                    ImportedSceneNode& targetNode = nodes[static_cast<std::size_t>(nodeObjectIndex)];
                    targetNode.mesh = std::move(meshData);
                    targetNode.material = std::move(material);
                    targetNode.boundsMin = boundsMin;
                    targetNode.boundsMax = boundsMax;
                    targetNode.hasMesh = true;
                }
                else
                {
                    ImportedSceneNode meshChild;
                    meshChild.parentIndex = nodeObjectIndex;
                    meshChild.name = nodes[static_cast<std::size_t>(nodeObjectIndex)].name + " (" + std::to_string(primitiveIndex + 1) + ")";
                    meshChild.mesh = std::move(meshData);
                    meshChild.material = std::move(material);
                    meshChild.boundsMin = boundsMin;
                    meshChild.boundsMax = boundsMax;
                    meshChild.hasMesh = true;
                    nodes.push_back(std::move(meshChild));
                }
            }
        }

        for (int childIndex : node.children)
        {
            VisitNode(
                model,
                childIndex,
                nodeObjectIndex,
                modelDirectory,
                projectRoot,
                textureCache,
                nodes,
                nameCounter,
                totalNodes,
                processedNodes,
                nodeProgressStart,
                loadMode,
                onProgress);
        }

        ++processedNodes;
        if (onProgress && totalNodes > 0)
        {
            const float nodeProgressSpan = 1.0f - nodeProgressStart;
            const float progress = nodeProgressStart
                + (nodeProgressSpan * static_cast<float>(processedNodes) / static_cast<float>(totalNodes));
            onProgress(progress, nodes[static_cast<std::size_t>(nodeObjectIndex)].name);
        }
    }

}

glm::mat4 GetImportedNodeWorldMatrix(
    const std::vector<ImportedSceneNode>& nodes,
    int nodeIndex)
{
    const ImportedSceneNode& node = nodes[static_cast<std::size_t>(nodeIndex)];
    const glm::mat4 localMatrix = node.transform.ToMatrix();
    if (node.parentIndex < 0)
    {
        return localMatrix;
    }

    return GetImportedNodeWorldMatrix(nodes, node.parentIndex) * localMatrix;
}

ImportedModel LoadModelFromFile(
    const std::string& path,
    const std::string& projectRoot,
    ModelOperationProgressFn onProgress,
    ModelLoadMode loadMode)
{
    ImportedModel importedModel;
    g_importTextureFailures = 0;
    g_importTexturesCached = 0;
    if (onProgress)
    {
        onProgress(0.0f, "Reading model file...");
    }

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(LoadImageData, nullptr);

    tinygltf::Model model;
    std::string error;
    std::string warning;

    const std::string extension = GetFileExtensionLower(path);
    const bool loaded = extension == ".glb"
        ? loader.LoadBinaryFromFile(&model, &error, &warning, path)
        : loader.LoadASCIIFromFile(&model, &error, &warning, path);

    if (!warning.empty())
    {
        importedModel.warningMessage = warning;
    }

    if (!loaded)
    {
        importedModel.errorMessage = error.empty() ? "Failed to load model file." : error;
        return importedModel;
    }

    const std::string modelDirectory = GetModelDirectory(path);
    std::unordered_map<std::string, std::shared_ptr<Texture>> textureCache;
    int nameCounter = 1;
    const int totalNodes = static_cast<int>(model.nodes.size());
    int processedNodes = 0;

    constexpr float kFullTextureExportStart = 0.05f;
    constexpr float kFullTextureExportEnd = 0.35f;
    const float nodeProgressStart = loadMode == ModelLoadMode::Full ? kFullTextureExportEnd : 0.05f;

    if (loadMode == ModelLoadMode::Full)
    {
        ExtractEmbeddedGltfImages(
            model,
            modelDirectory,
            [&](float localProgress, const std::string& detail) {
                if (onProgress)
                {
                    const float progress = kFullTextureExportStart
                        + (localProgress * (kFullTextureExportEnd - kFullTextureExportStart));
                    std::string message = "Writing embedded textures";
                    if (!detail.empty())
                    {
                        message += " — " + detail;
                    }
                    onProgress(progress, message);
                }
            });
    }

    if (onProgress)
    {
        const char* processingMessage = loadMode == ModelLoadMode::Full
            ? "Processing meshes and textures..."
            : "Processing meshes...";
        onProgress(nodeProgressStart, processingMessage);
    }

    ImportedSceneNode importRoot;
    importRoot.name = std::filesystem::path(path).stem().string();
    if (importRoot.name.empty())
    {
        importRoot.name = "Imported Model";
    }
    importRoot.parentIndex = -1;
    importedModel.rootNodeIndex = 0;
    importedModel.nodes.push_back(std::move(importRoot));

    if (!model.scenes.empty())
    {
        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
        const tinygltf::Scene& scene = model.scenes[static_cast<std::size_t>(sceneIndex)];
        for (int nodeIndex : scene.nodes)
        {
            VisitNode(
                model,
                nodeIndex,
                importedModel.rootNodeIndex,
                modelDirectory,
                projectRoot,
                textureCache,
                importedModel.nodes,
                nameCounter,
                totalNodes,
                processedNodes,
                nodeProgressStart,
                loadMode,
                onProgress);
        }
    }
    else
    {
        for (int nodeIndex = 0; nodeIndex < static_cast<int>(model.nodes.size()); ++nodeIndex)
        {
            VisitNode(
                model,
                nodeIndex,
                importedModel.rootNodeIndex,
                modelDirectory,
                projectRoot,
                textureCache,
                importedModel.nodes,
                nameCounter,
                totalNodes,
                processedNodes,
                nodeProgressStart,
                loadMode,
                onProgress);
        }
    }

    if (importedModel.nodes.size() <= 1)
    {
        importedModel.errorMessage = "No supported triangle meshes were found in the model.";
        importedModel.nodes.clear();
        importedModel.rootNodeIndex = -1;
        return importedModel;
    }

    importedModel.textureLoadFailures = g_importTextureFailures;
    importedModel.texturesCached = g_importTexturesCached;
    return importedModel;
}

bool EnsureGltfEmbeddedImagesOnDisk(
    const std::string& modelPath,
    std::string& outError,
    ModelOperationProgressFn onProgress)
{
    outError.clear();

    if (onProgress)
    {
        onProgress(0.0f, "Reading model file...");
    }

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(LoadImageData, nullptr);

    tinygltf::Model model;
    std::string error;
    std::string warning;

    const std::string extension = GetFileExtensionLower(modelPath);
    const bool loaded = extension == ".glb"
        ? loader.LoadBinaryFromFile(&model, &error, &warning, modelPath)
        : loader.LoadASCIIFromFile(&model, &error, &warning, modelPath);

    if (!loaded)
    {
        outError = error.empty() ? "Failed to load model file for texture export." : error;
        return false;
    }

    ExtractEmbeddedGltfImages(
        model,
        GetModelDirectory(modelPath),
        [&](float localProgress, const std::string& detail) {
            if (onProgress)
            {
                onProgress(0.1f + (localProgress * 0.9f), detail);
            }
        });
    return true;
}
