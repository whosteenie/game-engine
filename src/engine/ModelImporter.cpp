#include "engine/ModelImporter.h"

#include "engine/Constants.h"
#include "engine/Material.h"
#include "engine/Mesh.h"
#include "engine/Texture.h"
#include "engine/TextureCache.h"
#include "engine/TextureSamplerSettings.h"
#include "primitives/PrimitiveMeshUtils.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <stb_image.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace
{
    constexpr const char* ImportLogPrefix = "[glTF Import]";

    void ImportLog(const std::string& message)
    {
        std::cout << ImportLogPrefix << " " << message << std::endl;
    }

    std::string GltfWrapName(int wrap)
    {
        switch (wrap)
        {
        case 33071:
            return "CLAMP_TO_EDGE";
        case 33648:
            return "MIRRORED_REPEAT";
        case 10497:
            return "REPEAT";
        default:
            return "UNKNOWN(" + std::to_string(wrap) + ")";
        }
    }

    std::string GltfFilterName(int filter)
    {
        switch (filter)
        {
        case 9728:
            return "NEAREST";
        case 9729:
            return "LINEAR";
        case 9984:
            return "NEAREST_MIPMAP_NEAREST";
        case 9985:
            return "LINEAR_MIPMAP_NEAREST";
        case 9986:
            return "NEAREST_MIPMAP_LINEAR";
        case 9987:
            return "LINEAR_MIPMAP_LINEAR";
        default:
            return "UNKNOWN(" + std::to_string(filter) + ")";
        }
    }

    std::string GlWrapName(unsigned int wrap)
    {
        switch (wrap)
        {
        case 0x812F:
            return "GL_CLAMP_TO_EDGE";
        case 0x8370:
            return "GL_MIRRORED_REPEAT";
        case 0x2901:
            return "GL_REPEAT";
        default:
            return "GL_UNKNOWN(0x" + std::to_string(wrap) + ")";
        }
    }

    std::string GlFilterName(unsigned int filter)
    {
        switch (filter)
        {
        case 0x2600:
            return "GL_NEAREST";
        case 0x2601:
            return "GL_LINEAR";
        default:
            return "GL_UNKNOWN(0x" + std::to_string(filter) + ")";
        }
    }

    std::string FormatVec3(const glm::vec3& value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3)
            << "(" << value.x << ", " << value.y << ", " << value.z << ")";
        return stream.str();
    }

    std::string FormatVec2Range(const glm::vec2& minValue, const glm::vec2& maxValue)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(4)
            << "u[" << minValue.x << ", " << maxValue.x << "] v[" << minValue.y << ", " << maxValue.y << "]";
        return stream.str();
    }

    std::string FormatSamplerSettings(const TextureSamplerSettings& settings)
    {
        return GlWrapName(settings.wrapS) + "/" + GlWrapName(settings.wrapT) +
            " min=" + GlFilterName(settings.minFilter) +
            " mag=" + GlFilterName(settings.magFilter);
    }

    TextureSamplerSettings GetGltfSamplerSettings(const tinygltf::Model& model, int textureIndex);

    void LogResolvedSampler(const char* context, int textureIndex, const TextureSamplerSettings& settings)
    {
        std::ostringstream stream;
        stream << context << " texture #" << textureIndex << " -> " << FormatSamplerSettings(settings);
        ImportLog(stream.str());
    }

    void LogModelOverview(const tinygltf::Model& model, const std::string& path)
    {
        ImportLog("Loading: " + path);
        ImportLog(
            "Counts: scenes=" + std::to_string(model.scenes.size()) +
            " nodes=" + std::to_string(model.nodes.size()) +
            " meshes=" + std::to_string(model.meshes.size()) +
            " materials=" + std::to_string(model.materials.size()) +
            " textures=" + std::to_string(model.textures.size()) +
            " images=" + std::to_string(model.images.size()) +
            " samplers=" + std::to_string(model.samplers.size()));

        if (!model.extensionsUsed.empty())
        {
            std::ostringstream stream;
            stream << "Extensions used:";
            for (const std::string& extension : model.extensionsUsed)
            {
                stream << " " << extension;
            }
            ImportLog(stream.str());
        }

        for (std::size_t samplerIndex = 0; samplerIndex < model.samplers.size(); ++samplerIndex)
        {
            const tinygltf::Sampler& sampler = model.samplers[samplerIndex];
            std::ostringstream stream;
            stream << "Sampler #" << samplerIndex
                << " wrapS=" << GltfWrapName(sampler.wrapS)
                << " wrapT=" << GltfWrapName(sampler.wrapT)
                << " min=" << GltfFilterName(sampler.minFilter)
                << " mag=" << GltfFilterName(sampler.magFilter);
            ImportLog(stream.str());
        }

        for (std::size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex)
        {
            const tinygltf::Image& image = model.images[imageIndex];
            std::ostringstream stream;
            stream << "Image #" << imageIndex
                << " " << image.width << "x" << image.height
                << " ch=" << image.component;
            if (!image.uri.empty())
            {
                stream << " uri=\"" << image.uri << "\"";
            }
            else
            {
                stream << " embedded";
            }
            ImportLog(stream.str());
        }

        for (std::size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex)
        {
            const tinygltf::Texture& texture = model.textures[textureIndex];
            std::ostringstream stream;
            stream << "Texture #" << textureIndex
                << " image=" << texture.source
                << " sampler=" << texture.sampler
                << " resolved=" << FormatSamplerSettings(GetGltfSamplerSettings(model, static_cast<int>(textureIndex)));
            ImportLog(stream.str());
        }

        for (std::size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex)
        {
            const tinygltf::Material& material = model.materials[materialIndex];
            const auto& pbr = material.pbrMetallicRoughness;
            std::ostringstream stream;
            stream << "Material #" << materialIndex;
            if (!material.name.empty())
            {
                stream << " \"" << material.name << "\"";
            }
            stream << " baseColorFactor=" << FormatVec3(glm::vec3(
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2])));
            stream << " alphaMode=" << (material.alphaMode.empty() ? "OPAQUE" : material.alphaMode);
            stream << " doubleSided=" << (material.doubleSided ? "true" : "false");
            if (pbr.baseColorTexture.index >= 0)
            {
                stream << " baseColorTex=" << pbr.baseColorTexture.index
                    << " uvSet=" << pbr.baseColorTexture.texCoord;
            }
            else
            {
                stream << " baseColorTex=none";
            }
            if (pbr.metallicRoughnessTexture.index >= 0)
            {
                stream << " metallicRoughnessTex=" << pbr.metallicRoughnessTexture.index
                    << " uvSet=" << pbr.metallicRoughnessTexture.texCoord;
            }
            if (material.normalTexture.index >= 0)
            {
                stream << " normalTex=" << material.normalTexture.index
                    << " uvSet=" << material.normalTexture.texCoord;
            }
            if (material.occlusionTexture.index >= 0)
            {
                stream << " occlusionTex=" << material.occlusionTexture.index
                    << " uvSet=" << material.occlusionTexture.texCoord;
            }
            if (!material.extensions.empty())
            {
                stream << " extensions=" << material.extensions.size();
            }
            ImportLog(stream.str());
        }
    }

    void LogMeshPrimitive(
        const std::string& meshLabel,
        int materialIndex,
        int positionCount,
        std::size_t indexCount,
        bool hasNormals,
        bool hasTexCoord0,
        bool hasTexCoord1,
        bool hasTangents,
        bool hasColor0,
        bool generatedTangents,
        const glm::vec2& uv0Min,
        const glm::vec2& uv0Max,
        const glm::vec2& uv1Min,
        const glm::vec2& uv1Max,
        bool hasUv1Data)
    {
        std::ostringstream stream;
        stream << "Mesh \"" << meshLabel << "\" material=" << materialIndex
            << " verts=" << positionCount
            << " tris=" << (indexCount / 3)
            << " attrs:";
        if (hasNormals)
        {
            stream << " NORMAL";
        }
        if (hasTexCoord0)
        {
            stream << " TEXCOORD_0";
        }
        if (hasTexCoord1)
        {
            stream << " TEXCOORD_1";
        }
        if (hasTangents)
        {
            stream << " TANGENT";
        }
        if (hasColor0)
        {
            stream << " COLOR_0";
        }
        if (generatedTangents)
        {
            stream << " (generated tangents)";
        }
        if (hasTexCoord0)
        {
            stream << " uv0 " << FormatVec2Range(uv0Min, uv0Max);
        }
        if (hasUv1Data)
        {
            stream << " uv1 " << FormatVec2Range(uv1Min, uv1Max);
        }
        ImportLog(stream.str());

        if (hasColor0)
        {
            ImportLog("WARNING: Mesh \"" + meshLabel + "\" has COLOR_0 vertex colors — not applied by engine.");
        }

        if (hasTexCoord1 && materialIndex >= 0)
        {
            ImportLog("Note: Mesh \"" + meshLabel + "\" has TEXCOORD_1 — verify material uvSet assignments.");
        }
    }

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

    unsigned int ToGlWrap(int wrap)
    {
        switch (wrap)
        {
        case 33071:
            return 0x812F; // GL_CLAMP_TO_EDGE
        case 33648:
            return 0x8370; // GL_MIRRORED_REPEAT
        case 10497:
        default:
            return 0x2901; // GL_REPEAT
        }
    }

    unsigned int ToGlMinFilter(int filter)
    {
        switch (filter)
        {
        case 9728:
            return 0x2600; // GL_NEAREST
        case 9984:
            return 0x2700; // GL_NEAREST_MIPMAP_NEAREST
        case 9985:
            return 0x2701; // GL_LINEAR_MIPMAP_NEAREST
        case 9986:
            return 0x2702; // GL_NEAREST_MIPMAP_LINEAR
        case 9987:
            return 0x2703; // GL_LINEAR_MIPMAP_LINEAR
        case 9729:
        default:
            return 0x2601; // GL_LINEAR
        }
    }

    unsigned int ToGlMagFilter(int filter)
    {
        switch (filter)
        {
        case 9728:
            return 0x2600; // GL_NEAREST
        case 9729:
        default:
            return 0x2601; // GL_LINEAR
        }
    }

    TextureSamplerSettings GetGltfSamplerSettings(const tinygltf::Model& model, int textureIndex)
    {
        TextureSamplerSettings settings;
        settings.wrapS = 0x812F; // GL_CLAMP_TO_EDGE
        settings.wrapT = 0x812F;
        settings.minFilter = 0x2601; // GL_LINEAR (no mipmaps — avoids atlas bleed)
        settings.magFilter = 0x2601;

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
        settings.wrapS = ToGlWrap(sampler.wrapS);
        settings.wrapT = ToGlWrap(sampler.wrapT);
        settings.minFilter = ToGlMinFilter(sampler.minFilter);
        settings.magFilter = ToGlMagFilter(sampler.magFilter);

        // Mipmaps bleed between atlas islands; sample base level only.
        if (settings.minFilter == 0x2700 || settings.minFilter == 0x2702)
        {
            settings.minFilter = 0x2600; // GL_NEAREST
        }
        else if (settings.minFilter == 0x2701 || settings.minFilter == 0x2703)
        {
            settings.minFilter = 0x2601; // GL_LINEAR
        }

        // Atlases rely on edge clamping; glTF defaults to REPEAT which bleeds neighboring texels.
        settings.wrapS = 0x812F; // GL_CLAMP_TO_EDGE
        settings.wrapT = 0x812F;

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

    void GenerateTangents(
        std::vector<float>& vertices,
        const std::vector<unsigned int>& indices)
    {
        const unsigned int vertexCount = static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
        std::vector<glm::vec3> tan1(vertexCount, glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vertexCount, glm::vec3(0.0f));

        for (std::size_t triangleIndex = 0; triangleIndex + 2 < indices.size(); triangleIndex += 3)
        {
            const unsigned int index0 = indices[triangleIndex];
            const unsigned int index1 = indices[triangleIndex + 1];
            const unsigned int index2 = indices[triangleIndex + 2];

            const float* vertex0 = &vertices[static_cast<std::size_t>(index0) * Mesh::TexturedVertexFloatCount];
            const float* vertex1 = &vertices[static_cast<std::size_t>(index1) * Mesh::TexturedVertexFloatCount];
            const float* vertex2 = &vertices[static_cast<std::size_t>(index2) * Mesh::TexturedVertexFloatCount];

            const glm::vec3 position0(vertex0[0], vertex0[1], vertex0[2]);
            const glm::vec3 position1(vertex1[0], vertex1[1], vertex1[2]);
            const glm::vec3 position2(vertex2[0], vertex2[1], vertex2[2]);
            const glm::vec2 uv0(vertex0[6], vertex0[7]);
            const glm::vec2 uv1(vertex1[6], vertex1[7]);
            const glm::vec2 uv2(vertex2[6], vertex2[7]);

            const glm::vec3 edge1 = position1 - position0;
            const glm::vec3 edge2 = position2 - position0;
            const glm::vec2 deltaUv1 = uv1 - uv0;
            const glm::vec2 deltaUv2 = uv2 - uv0;

            const float determinant = deltaUv1.x * deltaUv2.y - deltaUv2.x * deltaUv1.y;
            if (std::abs(determinant) < 1.0e-8f)
            {
                continue;
            }

            const float inverseDeterminant = 1.0f / determinant;
            const glm::vec3 tangent =
                (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * inverseDeterminant;
            const glm::vec3 bitangent =
                (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * inverseDeterminant;

            tan1[index0] += tangent;
            tan1[index1] += tangent;
            tan1[index2] += tangent;
            tan2[index0] += bitangent;
            tan2[index1] += bitangent;
            tan2[index2] += bitangent;
        }

        for (unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            float* vertex = &vertices[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
            const glm::vec3 normal(vertex[3], vertex[4], vertex[5]);
            const glm::vec3 tangent = glm::normalize(tan1[vertexIndex] - normal * glm::dot(normal, tan1[vertexIndex]));
            const glm::vec3 bitangent = glm::normalize(tan2[vertexIndex] - normal * glm::dot(normal, tan2[vertexIndex]));
            const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
            vertex[10] = tangent.x;
            vertex[11] = tangent.y;
            vertex[12] = tangent.z;
            vertex[13] = handedness;
        }
    }

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
            ImportLog(
                "Loading external image #" + std::to_string(imageIndex) +
                " from \"" + texturePath + "\" " + FormatSamplerSettings(samplerSettings));
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
                ImportLog("FAILED to load external image #" + std::to_string(imageIndex) + ": " + exception.what());
                texture = nullptr;
            }
        }
        else if (!image.image.empty() && image.width > 0 && image.height > 0)
        {
            ImportLog(
                "Loading embedded image #" + std::to_string(imageIndex) +
                " " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                " ch=" + std::to_string(image.component) + " " + FormatSamplerSettings(samplerSettings));
            texture = Texture::CreateFromPixels(
                image.image.data(),
                image.width,
                image.height,
                image.component,
                colorSpace,
                samplerSettings,
                true);
        }
        else
        {
            ImportLog("FAILED image #" + std::to_string(imageIndex) + ": no pixel data available");
        }

        if (texture != nullptr && texture->IsValid())
        {
            textureCache.emplace(cacheKey, texture);
            ImportLog("Texture OK image #" + std::to_string(imageIndex) + " id=" + std::to_string(texture->GetId()));
        }
        else if (texture == nullptr || !texture->IsValid())
        {
            ImportLog("Texture INVALID for image #" + std::to_string(imageIndex));
        }

        return texture;
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
        LogResolvedSampler("Resolved", textureIndex, samplerSettings);
        return LoadGltfImageTexture(
            model,
            texture.source,
            modelDirectory,
            colorSpace,
            samplerSettings,
            textureCache);
    }

    std::shared_ptr<Texture> CreateRoughnessMapFromMetallicRoughness(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache)
    {
        if (textureIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        const int imageIndex = texture.source;
        if (imageIndex < 0)
        {
            return nullptr;
        }

        const TextureSamplerSettings samplerSettings = GetGltfSamplerSettings(model, textureIndex);
        const std::string cacheKey = MakeTextureCacheKey(imageIndex, samplerSettings, TextureColorSpace::Linear, "roughness:");
        const auto cachedTexture = textureCache.find(cacheKey);
        if (cachedTexture != textureCache.end())
        {
            return cachedTexture->second;
        }

        const tinygltf::Image& image = model.images[static_cast<std::size_t>(imageIndex)];
        if (image.width <= 0 || image.height <= 0 || image.component < 1)
        {
            return LoadGltfTexture(model, textureIndex, modelDirectory, TextureColorSpace::Linear, textureCache);
        }

        std::vector<unsigned char> roughnessPixels(
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
        for (int pixelIndex = 0; pixelIndex < image.width * image.height; ++pixelIndex)
        {
            const int sourceIndex = pixelIndex * image.component;
            const unsigned char greenChannel = image.component > 1 ? image.image[static_cast<std::size_t>(sourceIndex + 1)] : image.image[static_cast<std::size_t>(sourceIndex)];
            roughnessPixels[static_cast<std::size_t>(pixelIndex)] = greenChannel;
        }

        std::shared_ptr<Texture> roughnessTexture = Texture::CreateFromPixels(
            roughnessPixels.data(),
            image.width,
            image.height,
            1,
            TextureColorSpace::Linear,
            samplerSettings,
            true);

        if (roughnessTexture != nullptr && roughnessTexture->IsValid())
        {
            textureCache.emplace(cacheKey, roughnessTexture);
        }

        return roughnessTexture;
    }

    std::unique_ptr<Material> CreateMaterialFromGltf(
        const tinygltf::Model& model,
        int materialIndex,
        const std::string& modelDirectory,
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
                std::shared_ptr<Texture> albedoMap = LoadGltfTexture(
                    model,
                    pbr.baseColorTexture.index,
                    modelDirectory,
                    TextureColorSpace::SRGB,
                    textureCache);
                if (albedoMap == nullptr)
                {
                    ImportLog(
                        "WARNING: Material #" + std::to_string(materialIndex) +
                        " failed to load base color texture #" + std::to_string(pbr.baseColorTexture.index));
                }
                material->SetAlbedoMap(std::move(albedoMap));
            }
            else
            {
                ImportLog(
                    "WARNING: Material #" + std::to_string(materialIndex) +
                    " has no base color texture — using baseColorFactor " + FormatVec3(albedo));
            }

            if (pbr.metallicRoughnessTexture.index >= 0)
            {
                material->SetRoughnessTexCoordSet(pbr.metallicRoughnessTexture.texCoord);
                material->SetRoughnessMap(CreateRoughnessMapFromMetallicRoughness(
                    model,
                    pbr.metallicRoughnessTexture.index,
                    modelDirectory,
                    textureCache));
            }

            if (gltfMaterial.normalTexture.index >= 0)
            {
                material->SetNormalTexCoordSet(gltfMaterial.normalTexture.texCoord);
                material->SetNormalMap(LoadGltfTexture(
                    model,
                    gltfMaterial.normalTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    textureCache));
            }

            if (gltfMaterial.occlusionTexture.index >= 0)
            {
                material->SetAoTexCoordSet(gltfMaterial.occlusionTexture.texCoord);
                material->SetAoMap(LoadGltfTexture(
                    model,
                    gltfMaterial.occlusionTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    textureCache));
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
        glm::vec3& boundsMax,
        const std::string& meshLabel,
        int materialIndex)
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

        const bool hasColor0 = primitive.attributes.find("COLOR_0") != primitive.attributes.end();

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

        glm::vec2 uv0Min(std::numeric_limits<float>::max());
        glm::vec2 uv0Max(std::numeric_limits<float>::lowest());
        glm::vec2 uv1Min(std::numeric_limits<float>::max());
        glm::vec2 uv1Max(std::numeric_limits<float>::lowest());
        const bool hasTexCoord0 = texCoords != nullptr;
        const bool hasTexCoord1 = texCoords1 != nullptr;
        const bool hasTangents = tangents != nullptr;

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
                uv0Min = glm::min(uv0Min, uv0);
                uv0Max = glm::max(uv0Max, uv0);
            }

            glm::vec2 uv1(0.0f);
            if (texCoords1 != nullptr && vertexIndex < texCoord1Count)
            {
                uv1 = ReadVec2(texCoords1, vertexIndex);
                uv1Min = glm::min(uv1Min, uv1);
                uv1Max = glm::max(uv1Max, uv1);
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

        const bool generatedTangents = tangents == nullptr;
        if (generatedTangents)
        {
            GenerateTangents(vertices, indices);
        }

        LogMeshPrimitive(
            meshLabel,
            materialIndex,
            positionCount,
            indices.size(),
            normals != nullptr,
            hasTexCoord0,
            hasTexCoord1,
            hasTangents,
            hasColor0,
            generatedTangents,
            uv0Min,
            uv0Max,
            uv1Min,
            uv1Max,
            hasTexCoord1);

        outMesh = PrimitiveMesh::BuildMesh(vertices, indices);
        return outMesh != nullptr;
    }

    void VisitNode(
        const tinygltf::Model& model,
        int nodeIndex,
        int parentNodeIndex,
        const std::string& modelDirectory,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache,
        std::vector<ImportedSceneNode>& nodes,
        int& nameCounter)
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
                const std::string meshLabel = nodes[static_cast<std::size_t>(nodeObjectIndex)].name +
                    " [prim " + std::to_string(primitiveIndex + 1) + "]";
                if (!BuildMeshFromPrimitive(
                    model,
                    primitive,
                    meshData,
                    boundsMin,
                    boundsMax,
                    meshLabel,
                    primitive.material))
                {
                    continue;
                }

                auto material = CreateMaterialFromGltf(
                    model,
                    primitive.material,
                    modelDirectory,
                    textureCache);

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
                textureCache,
                nodes,
                nameCounter);
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

ImportedModel LoadModelFromFile(const std::string& path)
{
    ImportedModel importedModel;
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
        ImportLog("Loader warning: " + warning);
    }

    if (!loaded)
    {
        importedModel.errorMessage = error.empty() ? "Failed to load model file." : error;
        return importedModel;
    }

    LogModelOverview(model, path);

    const std::string modelDirectory = GetModelDirectory(path);
    std::unordered_map<std::string, std::shared_ptr<Texture>> textureCache;
    int nameCounter = 1;

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
                textureCache,
                importedModel.nodes,
                nameCounter);
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
                textureCache,
                importedModel.nodes,
                nameCounter);
        }
    }

    if (importedModel.nodes.size() <= 1)
    {
        importedModel.errorMessage = "No supported triangle meshes were found in the model.";
        importedModel.nodes.clear();
        importedModel.rootNodeIndex = -1;
        ImportLog("Import failed: no supported triangle meshes found.");
        return importedModel;
    }

    ImportLog(
        "Import complete: " + std::to_string(importedModel.nodes.size() - 1) +
        " scene object(s), " + std::to_string(textureCache.size()) + " GPU texture(s) created.");
    return importedModel;
}
