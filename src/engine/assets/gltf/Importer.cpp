#define GLTF_DETAIL_IMPLEMENTATION
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

using namespace GltfDetail;

ImportedModel LoadModelFromFile(
    const std::string& path,
    const std::string& projectRoot,
    ModelOperationProgressFn onProgress,
    ModelLoadMode loadMode)
{
    ImportedModel importedModel;
    ResetTextureStats();
    
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
    bool loaded = false;
    {
        ProjectLoadBenchmark::ScopedPhase parseModelPhase(
            loadMode == ModelLoadMode::GeometryOnly
                ? "project.deserialize.imported_model_parse"
                : nullptr);
        loaded = extension == ".glb"
            ? loader.LoadBinaryFromFile(&model, &error, &warning, path)
            : loader.LoadASCIIFromFile(&model, &error, &warning, path);
    }

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
        ExtractEmbeddedImages(
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

    importedModel.textureLoadFailures = GetTextureLoadFailureCount();
    importedModel.texturesCached = GetTextureCacheHitCount();
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

    ExtractEmbeddedImages(
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
