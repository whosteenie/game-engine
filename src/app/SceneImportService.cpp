#include "app/SceneImportService.h"

#include "app/Scene.h"
#include "app/SceneHierarchyOps.h"
#include "app/SceneMeshLibrary.h"
#include "app/SceneObjectStore.h"
#include "app/SceneSpawnService.h"

#include "engine/ModelImporter.h"
#include "engine/Mesh.h"
#include "engine/NativeProgressWindow.h"
#include "engine/ProjectAssets.h"
#include "engine/SceneHierarchy.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

std::vector<int> SceneImportService::ImportModel(
    Scene& scene,
    const std::string& path,
    int parentIndex,
    const std::string& projectRoot)
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    const std::string modelName = std::filesystem::path(path).filename().string();
    ScopedNativeProgress progress("Importing Model", modelName);

    std::string importPath = path;
    if (!projectRoot.empty())
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

    const bool placeAtSceneRoot = parentIndex < 0;
    const float spread = placeAtSceneRoot
        ? static_cast<float>(scene.GetSpawnService().AllocateImportNumber()) * 2.5f
        : 0.0f;

    float minWorldY = std::numeric_limits<float>::max();
    const glm::mat4 sceneParentWorld = parentIndex >= 0 ? scene.GetWorldMatrix(parentIndex) : glm::mat4(1.0f);
    for (std::size_t nodeIndex = 0; nodeIndex < importedModel.nodes.size(); ++nodeIndex)
    {
        const ImportedSceneNode& node = importedModel.nodes[nodeIndex];
        if (!node.hasMesh)
        {
            continue;
        }

        glm::mat4 worldMatrix = GetImportedNodeWorldMatrix(importedModel.nodes, static_cast<int>(nodeIndex));
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
    importedModel.nodes[static_cast<std::size_t>(importedModel.rootNodeIndex)].transform.position +=
        glm::vec3(spread, floorOffset, 0.0f);

    const int baseObjectIndex = static_cast<int>(scene.GetObjectStore().Objects().size());
    std::vector<int> importedSceneIndices;
    importedSceneIndices.reserve(importedModel.nodes.size());

    for (std::size_t importedNodeIndex = 0; importedNodeIndex < importedModel.nodes.size(); ++importedNodeIndex)
    {
        ImportedSceneNode& node = importedModel.nodes[importedNodeIndex];
        Mesh* mesh = nullptr;
        if (node.hasMesh && node.mesh != nullptr)
        {
            mesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(node.mesh));
        }

        int parentSceneIndex = -1;
        if (node.parentIndex >= 0)
        {
            parentSceneIndex = importedSceneIndices[static_cast<std::size_t>(node.parentIndex)];
        }
        else if (static_cast<int>(importedNodeIndex) == importedModel.rootNodeIndex && parentIndex >= 0)
        {
            parentSceneIndex = parentIndex;
        }

        scene.GetObjectStore().Objects().emplace_back(
            node.name,
            mesh,
            node.hasMesh ? std::move(node.material) : nullptr,
            node.hasMesh ? node.boundsMin : glm::vec3(0.0f),
            node.hasMesh ? node.boundsMax : glm::vec3(0.0f),
            node.transform,
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
    return {baseObjectIndex + importedModel.rootNodeIndex};
}

const std::string& SceneImportService::GetLastImportError() const
{
    return m_lastImportError;
}

const std::string& SceneImportService::GetLastImportWarning() const
{
    return m_lastImportWarning;
}

void SceneImportService::ClearMessages()
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();
}
