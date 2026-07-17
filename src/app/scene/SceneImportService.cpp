#include "app/scene/SceneImportService.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneHierarchyOps.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectStore.h"
#include "app/scene/SceneSpawnService.h"

#include "engine/assets/ModelImporter.h"
#include "engine/platform/RenderPathDiagnostics.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/platform/NativeProgressWindow.h"
#include "engine/assets/ProjectAssets.h"
#include "engine/scene/SceneHierarchy.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    std::string CanonicalizeImportPath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
        return (error ? path : canonicalPath).string();
    }

    std::string MakeImportCacheKey(
        const std::filesystem::path& path,
        const std::string& projectRoot = {},
        const bool allowProjectAssetsFallback = false)
    {
        std::string key = std::filesystem::path(CanonicalizeImportPath(path)).generic_string();
#ifdef _WIN32
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
#endif

        if (projectRoot.empty())
        {
            return "absolute:" + key;
        }

        std::string rootKey =
            std::filesystem::path(CanonicalizeImportPath(projectRoot)).generic_string();
#ifdef _WIN32
        std::transform(rootKey.begin(), rootKey.end(), rootKey.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
#endif
        while (rootKey.size() > 1 && rootKey.back() == '/')
        {
            rootKey.pop_back();
        }

        if (key.size() > rootKey.size()
            && key.compare(0, rootKey.size(), rootKey) == 0
            && key[rootKey.size()] == '/')
        {
            return "project:" + key.substr(rootKey.size() + 1);
        }

        if (allowProjectAssetsFallback)
        {
#ifdef _WIN32
            constexpr const char* projectAssetsMarker = "/assets/models/";
#else
            constexpr const char* projectAssetsMarker = "/Assets/Models/";
#endif
            const std::size_t assetsMarker = key.find(projectAssetsMarker);
            if (assetsMarker != std::string::npos)
            {
                return "project:" + key.substr(assetsMarker + 1);
            }
        }

        return "absolute:" + key;
    }

    std::string FormatVec3(const glm::vec3& value)
    {
        std::ostringstream stream;
        stream << "(" << value.x << ", " << value.y << ", " << value.z << ")";
        return stream.str();
    }

    std::string SnapshotTrackedObjects(const Scene& scene, const char* label)
    {
        std::ostringstream stream;
        stream << label << " tracked objects:";
        const std::vector<SceneObject>& objects = scene.GetObjects();
        int logged = 0;
        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            const SceneObject& object = objects[objectIndex];
            if (object.GetName() != "Cube" && object.GetName() != "Floor" && object.GetImportAssetPath().empty())
            {
                continue;
            }

            const glm::mat4 worldMatrix = scene.GetWorldMatrix(static_cast<int>(objectIndex));
            const glm::vec3 worldPosition = glm::vec3(worldMatrix[3]);
            stream << "\n  [" << objectIndex << "] \"" << object.GetName() << "\""
                   << " pos=" << FormatVec3(worldPosition)
                   << " mesh=" << (object.HasMesh() ? "yes" : "no");
            if (object.HasMaterial())
            {
                const Material& material = object.GetMaterial();
                stream << " albedo=" << FormatVec3(material.GetAlbedo())
                       << " albedoMap=" << (material.HasAlbedoMap() ? "assigned" : "none");
            }
            else
            {
                stream << " material=none";
            }
            stream << " import=" << (object.GetImportAssetPath().empty() ? "no" : "yes");
            ++logged;
            if (logged >= 8)
            {
                break;
            }
        }

        if (logged == 0)
        {
            stream << " (none)";
        }

        return stream.str();
    }
}

bool SceneImportService::IsCachedModelUsable(
    const Scene& scene,
    const CachedImportedModel& cachedModel) const
{
    if (cachedModel.nodes.empty() || cachedModel.rootNodeIndex < 0)
    {
        return false;
    }

    for (const CachedImportedNode& node : cachedModel.nodes)
    {
        if (node.hasMesh && !scene.GetMeshLibrary().IsImportedMesh(node.mesh))
        {
            return false;
        }
    }

    return true;
}

SceneImportService::CachedImportedModel SceneImportService::BuildCachedModel(
    Scene& scene,
    ImportedModel& importedModel)
{
    CachedImportedModel cachedModel;
    cachedModel.rootNodeIndex = importedModel.rootNodeIndex;
    cachedModel.warningMessage = importedModel.warningMessage;
    cachedModel.textureLoadFailures = importedModel.textureLoadFailures;
    cachedModel.texturesCached = importedModel.texturesCached;
    cachedModel.nodes.reserve(importedModel.nodes.size());

    for (ImportedSceneNode& node : importedModel.nodes)
    {
        Mesh* mesh = nullptr;
        if (node.hasMesh && node.mesh != nullptr)
        {
            mesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(node.mesh));
            scene.GetMeshLibrary().PinImportedMesh(mesh);
        }

        CachedImportedNode cachedNode;
        cachedNode.name = node.name;
        cachedNode.parentIndex = node.parentIndex;
        cachedNode.transform = node.transform;
        cachedNode.mesh = mesh;
        cachedNode.material = node.material != nullptr ? node.material->Clone() : nullptr;
        cachedNode.boundsMin = node.boundsMin;
        cachedNode.boundsMax = node.boundsMax;
        cachedNode.hasMesh = node.hasMesh;
        cachedModel.nodes.push_back(std::move(cachedNode));
    }

    return cachedModel;
}

SceneImportService::CachedImportedModel SceneImportService::BuildCachedModelFromLoadedGeometry(
    Scene& scene,
    const std::string& importPath,
    ImportedModel&& geometryModel)
{
    std::unordered_map<int, const SceneObject*> objectsByNodeIndex;
    for (const SceneObject& object : scene.GetObjects())
    {
        if (object.GetImportNodeIndex() < 0)
        {
            continue;
        }

        const std::string objectImportPath = MakeImportCacheKey(object.GetImportAssetPath());
        if (objectImportPath != importPath)
        {
            continue;
        }

        objectsByNodeIndex.try_emplace(object.GetImportNodeIndex(), &object);
    }

    CachedImportedModel cachedModel;
    cachedModel.rootNodeIndex = geometryModel.rootNodeIndex;
    cachedModel.warningMessage = geometryModel.warningMessage;
    cachedModel.nodes.reserve(geometryModel.nodes.size());

    for (std::size_t nodeIndex = 0; nodeIndex < geometryModel.nodes.size(); ++nodeIndex)
    {
        const ImportedSceneNode& node = geometryModel.nodes[nodeIndex];
        const auto objectIterator = objectsByNodeIndex.find(static_cast<int>(nodeIndex));
        const SceneObject* sourceObject =
            objectIterator != objectsByNodeIndex.end() ? objectIterator->second : nullptr;

        if (node.hasMesh && (sourceObject == nullptr || !sourceObject->HasMesh()))
        {
            return {};
        }

        CachedImportedNode cachedNode;
        cachedNode.name = node.name;
        cachedNode.parentIndex = node.parentIndex;
        cachedNode.transform = node.transform;
        cachedNode.mesh = node.hasMesh ? sourceObject->GetMesh() : nullptr;
        cachedNode.material = sourceObject != nullptr && sourceObject->HasMaterial()
            ? sourceObject->GetMaterial().Clone()
            : nullptr;
        cachedNode.boundsMin = sourceObject != nullptr
            ? sourceObject->GetLocalBoundsMin()
            : node.boundsMin;
        cachedNode.boundsMax = sourceObject != nullptr
            ? sourceObject->GetLocalBoundsMax()
            : node.boundsMax;
        cachedNode.hasMesh = node.hasMesh;
        cachedModel.nodes.push_back(std::move(cachedNode));
    }

    for (const CachedImportedNode& node : cachedModel.nodes)
    {
        if (node.mesh != nullptr)
        {
            scene.GetMeshLibrary().PinImportedMesh(node.mesh);
        }
    }

    return cachedModel;
}

void SceneImportService::CacheLoadedProjectModel(
    Scene& scene,
    const std::string& importPath,
    ImportedModel&& geometryModel)
{
    if (importPath.empty() || geometryModel.nodes.empty() || geometryModel.rootNodeIndex < 0)
    {
        return;
    }

    const std::string cachePath = MakeImportCacheKey(importPath);
    CachedImportedModel cachedModel =
        BuildCachedModelFromLoadedGeometry(scene, cachePath, std::move(geometryModel));
    if (IsCachedModelUsable(scene, cachedModel))
    {
        m_cachedModels.insert_or_assign(cachePath, std::move(cachedModel));
    }
}

std::vector<int> SceneImportService::InstantiateCachedModel(
    Scene& scene,
    const std::string& importPath,
    const CachedImportedModel& cachedModel,
    int parentIndex)
{
    const bool placeAtSceneRoot = parentIndex < 0;
    const float spread = placeAtSceneRoot
        ? static_cast<float>(scene.GetSpawnService().AllocateImportNumber()) * 2.5f
        : 0.0f;

    float minWorldY = std::numeric_limits<float>::max();
    const glm::mat4 sceneParentWorld = parentIndex >= 0 ? scene.GetWorldMatrix(parentIndex) : glm::mat4(1.0f);
    const std::function<glm::mat4(int)> getCachedNodeWorldMatrix = [&](int nodeIndex) {
        const CachedImportedNode& node = cachedModel.nodes[static_cast<std::size_t>(nodeIndex)];
        const glm::mat4 localMatrix = node.transform.ToMatrix();
        if (node.parentIndex < 0)
        {
            return localMatrix;
        }

        return getCachedNodeWorldMatrix(node.parentIndex) * localMatrix;
    };

    for (std::size_t nodeIndex = 0; nodeIndex < cachedModel.nodes.size(); ++nodeIndex)
    {
        const CachedImportedNode& node = cachedModel.nodes[nodeIndex];
        if (!node.hasMesh)
        {
            continue;
        }

        glm::mat4 worldMatrix = getCachedNodeWorldMatrix(static_cast<int>(nodeIndex));
        if (parentIndex >= 0)
        {
            worldMatrix = sceneParentWorld * worldMatrix;
        }

        const glm::vec3 corners[2] = {node.boundsMin, node.boundsMax};
        for (const glm::vec3& corner : corners)
        {
            const glm::vec4 worldCorner = worldMatrix * glm::vec4(corner, 1.0f);
            minWorldY = std::min(minWorldY, worldCorner.y);
        }
    }

    const float floorOffset = minWorldY < std::numeric_limits<float>::max() ? -minWorldY : 0.0f;

    const int baseObjectIndex = static_cast<int>(scene.GetObjectStore().Objects().size());
    std::vector<int> importedSceneIndices;
    importedSceneIndices.reserve(cachedModel.nodes.size());

    for (std::size_t importedNodeIndex = 0; importedNodeIndex < cachedModel.nodes.size(); ++importedNodeIndex)
    {
        const CachedImportedNode& node = cachedModel.nodes[importedNodeIndex];
        Transform transform = node.transform;
        if (static_cast<int>(importedNodeIndex) == cachedModel.rootNodeIndex)
        {
            transform.position += glm::vec3(spread, floorOffset, 0.0f);
        }

        int parentSceneIndex = -1;
        if (node.parentIndex >= 0)
        {
            parentSceneIndex = importedSceneIndices[static_cast<std::size_t>(node.parentIndex)];
        }
        else if (static_cast<int>(importedNodeIndex) == cachedModel.rootNodeIndex && parentIndex >= 0)
        {
            parentSceneIndex = parentIndex;
        }

        scene.GetObjectStore().Objects().emplace_back(
            node.name,
            node.mesh,
            node.material != nullptr ? node.material->Clone() : nullptr,
            node.hasMesh ? node.boundsMin : glm::vec3(0.0f),
            node.hasMesh ? node.boundsMax : glm::vec3(0.0f),
            transform,
            node.hasMesh,
            node.hasMesh,
            parentSceneIndex,
            SceneHierarchyOps::AllocateSiblingOrder(scene.GetObjectStore().Objects(), parentSceneIndex));

        importedSceneIndices.push_back(static_cast<int>(scene.GetObjectStore().Objects().size()) - 1);

        SceneObject& createdObject = scene.GetObjectStore().Objects().back();
        createdObject.SetImportSource(importPath, static_cast<int>(importedNodeIndex));
        scene.GetObjectStore().FinalizeNewObject(createdObject);
    }

    scene.MarkDirty();
    return {baseObjectIndex + cachedModel.rootNodeIndex};
}

std::vector<int> SceneImportService::ImportModel(
    Scene& scene,
    const std::string& path,
    int parentIndex,
    const std::string& projectRoot,
    const bool isProjectAsset)
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    RenderPathDiagnostics::LogImportBegin(path, parentIndex);
    const std::string objectsBeforeImport = SnapshotTrackedObjects(scene, "before");

    const std::string modelName = std::filesystem::path(path).filename().string();

    const std::string sourceCachePath =
        MakeImportCacheKey(path, projectRoot, isProjectAsset);
    if (!sourceCachePath.empty())
    {
        const auto aliasIterator = m_sourceAssetAliases.find(sourceCachePath);
        const std::string& cachePath = aliasIterator != m_sourceAssetAliases.end()
            ? aliasIterator->second
            : sourceCachePath;
        const auto cachedIterator = m_cachedModels.find(cachePath);
        if (cachedIterator != m_cachedModels.end())
        {
            if (IsCachedModelUsable(scene, cachedIterator->second))
            {
                m_lastImportWarning = cachedIterator->second.warningMessage;
                return InstantiateCachedModel(scene, cachePath, cachedIterator->second, parentIndex);
            }

            m_cachedModels.erase(cachedIterator);
        }
    }

    ScopedNativeProgress progress("Importing Model", modelName);

    std::string importPath = CanonicalizeImportPath(path);
    if (!projectRoot.empty() && !isProjectAsset)
    {
        progress.SetMessage("Copying model into project...");
        const ImportModelAssetResult assetResult = ImportModelToProject(path, projectRoot);
        if (!assetResult.success)
        {
            m_lastImportError = assetResult.errorMessage.empty()
                ? "Failed to copy model into project assets."
                : assetResult.errorMessage;
            return {};
        }

        importPath = assetResult.absolutePath;
    }

    importPath = CanonicalizeImportPath(importPath);
    const std::string importCachePath =
        MakeImportCacheKey(importPath, projectRoot, isProjectAsset);

    const auto cachedIterator = m_cachedModels.find(importCachePath);
    if (cachedIterator != m_cachedModels.end())
    {
        if (IsCachedModelUsable(scene, cachedIterator->second))
        {
            m_lastImportWarning = cachedIterator->second.warningMessage;
            return InstantiateCachedModel(scene, importPath, cachedIterator->second, parentIndex);
        }

        m_cachedModels.erase(cachedIterator);
    }

    progress.SetMessage("Loading meshes and textures...");
    ImportedModel importedModel = LoadModelFromFile(
        importPath,
        projectRoot,
        [&](float loadProgress, const std::string& detail) {
            progress.SetProgress(loadProgress);
            if (!detail.empty())
            {
                progress.SetMessage("Loading meshes and textures - " + detail);
            }
        });
    if (!importedModel.errorMessage.empty())
    {
        m_lastImportError = importedModel.errorMessage;
        return {};
    }

    m_lastImportWarning = importedModel.warningMessage;

    if (importedModel.nodes.empty() || importedModel.rootNodeIndex < 0)
    {
        m_lastImportError = "No meshes were imported from the model.";
        return {};
    }

    int meshNodes = 0;
    for (const ImportedSceneNode& node : importedModel.nodes)
    {
        if (node.hasMesh)
        {
            ++meshNodes;
        }
    }

    CachedImportedModel cachedModel = BuildCachedModel(scene, importedModel);
    auto [cacheIterator, inserted] = m_cachedModels.insert_or_assign(importCachePath, std::move(cachedModel));
    if (!sourceCachePath.empty() && sourceCachePath != importCachePath)
    {
        m_sourceAssetAliases.insert_or_assign(sourceCachePath, importCachePath);
    }
    const std::vector<int> importedIndices =
        InstantiateCachedModel(scene, importPath, cacheIterator->second, parentIndex);

    std::uint32_t srvUsed = 0;
    std::uint32_t srvCapacity = 0;
    GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
    RenderPathDiagnostics::LogImportComplete(
        importPath,
        static_cast<int>(importedModel.nodes.size()),
        meshNodes,
        importedModel.texturesCached,
        importedModel.textureLoadFailures,
        0.0f,
        srvUsed,
        srvCapacity,
        objectsBeforeImport + "\n" + SnapshotTrackedObjects(scene, "after"));

    return importedIndices;
}

const std::string& SceneImportService::GetLastImportError() const
{
    return m_lastImportError;
}

const std::string& SceneImportService::GetLastImportWarning() const
{
    return m_lastImportWarning;
}

int SceneImportService::PrewarmProjectModels(
    Scene& scene,
    const std::string& projectRoot,
    float progressStart,
    float progressEnd)
{
    if (projectRoot.empty())
    {
        return 0;
    }

    namespace fs = std::filesystem;
    const fs::path modelsRoot = fs::path(projectRoot) / "Assets" / "Models";
    std::error_code error;
    if (!fs::exists(modelsRoot, error) || !fs::is_directory(modelsRoot, error))
    {
        return 0;
    }

    std::vector<fs::path> modelPaths;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(modelsRoot, fs::directory_options::skip_permission_denied, error))
    {
        if (error)
        {
            error.clear();
            continue;
        }

        if (!entry.is_regular_file(error))
        {
            continue;
        }

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extension == ".gltf" || extension == ".glb")
        {
            modelPaths.push_back(entry.path());
        }
    }

    if (modelPaths.empty())
    {
        return 0;
    }

    std::sort(modelPaths.begin(), modelPaths.end());
    progressStart = std::clamp(progressStart, 0.0f, 1.0f);
    progressEnd = std::clamp(progressEnd, progressStart, 1.0f);
    const float progressSpan = progressEnd - progressStart;

    int warmedCount = 0;
    for (std::size_t modelIndex = 0; modelIndex < modelPaths.size(); ++modelIndex)
    {
        const fs::path& modelPath = modelPaths[modelIndex];
        const std::string importPath = CanonicalizeImportPath(modelPath);
        const std::string importCachePath = MakeImportCacheKey(importPath, projectRoot, true);

        const auto cachedIterator = m_cachedModels.find(importCachePath);
        if (cachedIterator != m_cachedModels.end() && IsCachedModelUsable(scene, cachedIterator->second))
        {
            ++warmedCount;
            const float progress = progressStart
                + progressSpan * (static_cast<float>(modelIndex + 1) / static_cast<float>(modelPaths.size()));
            NativeProgressWindow::Instance().Report({}, progress);
            continue;
        }

        if (cachedIterator != m_cachedModels.end())
        {
            m_cachedModels.erase(cachedIterator);
        }

        NativeProgressWindow::Instance().Report(
            "Prewarming " + modelPath.filename().string() + " (" +
                std::to_string(modelIndex + 1) + "/" + std::to_string(modelPaths.size()) + ")",
            progressStart
                + progressSpan * (static_cast<float>(modelIndex) / static_cast<float>(modelPaths.size())));

        ImportedModel importedModel = LoadModelFromFile(
            importPath,
            projectRoot,
            [&](float localProgress, const std::string& detail) {
                const float modelStart = static_cast<float>(modelIndex) / static_cast<float>(modelPaths.size());
                const float modelSpan = 1.0f / static_cast<float>(modelPaths.size());
                const float progress = progressStart + progressSpan * (modelStart + localProgress * modelSpan);
                if (!detail.empty())
                {
                    NativeProgressWindow::Instance().Report(
                        "Prewarming " + modelPath.filename().string() + " - " + detail,
                        progress);
                }
                else
                {
                    NativeProgressWindow::Instance().SetProgress(progress);
                }
            });

        if (!importedModel.errorMessage.empty()
            || importedModel.nodes.empty()
            || importedModel.rootNodeIndex < 0)
        {
            const float progress = progressStart
                + progressSpan * (static_cast<float>(modelIndex + 1) / static_cast<float>(modelPaths.size()));
            NativeProgressWindow::Instance().Report({}, progress);
            continue;
        }

        CachedImportedModel cachedModel = BuildCachedModel(scene, importedModel);
        m_cachedModels.insert_or_assign(importCachePath, std::move(cachedModel));
        ++warmedCount;
        const float progress = progressStart
            + progressSpan * (static_cast<float>(modelIndex + 1) / static_cast<float>(modelPaths.size()));
        NativeProgressWindow::Instance().Report({}, progress);
    }

    NativeProgressWindow::Instance().SetProgress(progressEnd);
    return warmedCount;
}

void SceneImportService::ClearMessages()
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();
}

void SceneImportService::ClearCache()
{
    m_cachedModels.clear();
    m_sourceAssetAliases.clear();
}
