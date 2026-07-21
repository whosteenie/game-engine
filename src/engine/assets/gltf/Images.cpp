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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace GltfDetail
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

    void ExtractEmbeddedImages(
        const tinygltf::Model& model,
        const std::string& modelDirectory,
        const ModelOperationProgressFn& onProgress)
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

    void ResetTextureStats()
    {
        g_importTextureFailures = 0;
        g_importTexturesCached = 0;
    }

    int GetTextureLoadFailureCount()
    {
        return g_importTextureFailures;
    }

    int GetTextureCacheHitCount()
    {
        return g_importTexturesCached;
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

}
