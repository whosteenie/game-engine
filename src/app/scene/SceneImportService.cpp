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
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace
{
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

std::vector<int> SceneImportService::ImportModel(
    Scene& scene,
    const std::string& path,
    int parentIndex,
    const std::string& projectRoot)
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    RenderPathDiagnostics::LogImportBegin(path, parentIndex);
    const std::string objectsBeforeImport = SnapshotTrackedObjects(scene, "before");

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

    int meshNodes = 0;
    for (const ImportedSceneNode& node : importedModel.nodes)
    {
        if (node.hasMesh)
        {
            ++meshNodes;
        }
    }

    std::uint32_t srvUsed = 0;
    std::uint32_t srvCapacity = 0;
    GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
    RenderPathDiagnostics::LogImportComplete(
        importPath,
        static_cast<int>(importedModel.nodes.size()),
        meshNodes,
        importedModel.texturesCached,
        importedModel.textureLoadFailures,
        floorOffset,
        srvUsed,
        srvCapacity,
        objectsBeforeImport + "\n" + SnapshotTrackedObjects(scene, "after"));

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
