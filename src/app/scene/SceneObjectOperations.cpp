#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectOperations.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneHierarchyOps.h"
#include "app/scene/SceneObjectStore.h"
#include "app/scene/SceneSelectionController.h"
#include "engine/components/CameraComponent.h"
#include "engine/scene/SceneObjectComponents.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/scene/SceneObject.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

bool SceneObjectOperations::RemoveObject(Scene& scene, std::size_t index)
{
    std::vector<SceneObject>& objects = scene.GetObjectStore().Objects();
    if (index >= objects.size())
    {
        return false;
    }

    std::vector<int> indicesToRemove;
    SceneHierarchyOps::CollectDescendantIndices(objects, static_cast<int>(index), indicesToRemove);
    std::sort(indicesToRemove.begin(), indicesToRemove.end(), std::greater<int>());

    for (int removeIndex : indicesToRemove)
    {
        objects.erase(objects.begin() + removeIndex);
        SceneHierarchyOps::RemapParentIndicesAfterRemoval(objects, removeIndex);
        scene.GetSelectionController().RemapAfterRemoval(removeIndex);
    }

    scene.GetSelectionController().Sanitize(objects);
    scene.GetMeshLibrary().PruneUnusedImportedMeshes(objects);
    scene.MarkDirty();
    return true;
}

bool SceneObjectOperations::RemoveSelectedObjects(Scene& scene)
{
    if (!scene.HasSelection())
    {
        return false;
    }

    std::vector<SceneObject>& objects = scene.GetObjectStore().Objects();
    std::vector<int> rootsToRemove =
        FilterToTopmostSelectedIndices(objects, scene.GetSelection().indices);
    if (rootsToRemove.empty())
    {
        return false;
    }

    std::sort(rootsToRemove.begin(), rootsToRemove.end(), std::greater<int>());
    for (int objectIndex : rootsToRemove)
    {
        RemoveObject(scene, static_cast<std::size_t>(objectIndex));
    }

    return true;
}

std::string SceneObjectOperations::MakeDuplicateObjectName(
    const Scene& scene,
    const std::string& sourceName)
{
    auto nameExists = [&scene](const std::string& name) {
        for (const SceneObject& object : scene.GetObjects())
        {
            if (object.GetName() == name)
            {
                return true;
            }
        }

        return false;
    };

    for (int suffix = 1; suffix < 1000; ++suffix)
    {
        const std::string candidate = sourceName + " (" + std::to_string(suffix) + ")";
        if (!nameExists(candidate))
        {
            return candidate;
        }
    }

    return sourceName + " (copy)";
}

int SceneObjectOperations::DuplicateObject(Scene& scene, int objectIndex)
{
    std::vector<SceneObject>& objects = scene.GetObjectStore().Objects();
    if (objectIndex < 0 || objectIndex >= static_cast<int>(objects.size()))
    {
        return -1;
    }

    std::vector<int> sourceIndices;
    SceneHierarchyOps::CollectDescendantIndices(objects, objectIndex, sourceIndices);

    std::unordered_map<int, int> indexMap;
    int duplicateRootIndex = -1;

    for (int sourceIndex : sourceIndices)
    {
        const SceneObject& source = objects[static_cast<std::size_t>(sourceIndex)];

        int newParentIndex = -1;
        const int sourceParentIndex = source.GetParentIndex();
        if (sourceIndex == objectIndex)
        {
            newParentIndex = sourceParentIndex;
        }
        else if (sourceParentIndex >= 0)
        {
            newParentIndex = indexMap.at(sourceParentIndex);
        }

        SceneObjectComponentSnapshot components = CaptureSceneObjectComponents(source);

        std::string objectName = source.GetName();
        if (sourceIndex == objectIndex)
        {
            objectName = MakeDuplicateObjectName(scene, source.GetName());
        }

        if (sourceIndex == objectIndex && components.camera.has_value() && components.camera->isMain)
        {
            components.camera->isMain = false;
        }

        objects.emplace_back(
            objectName,
            source.GetMesh(),
            std::move(components.material),
            source.GetLocalBoundsMin(),
            source.GetLocalBoundsMax(),
            source.GetTransform(),
            source.CastsShadow(),
            source.ReceivesShadow(),
            newParentIndex,
            source.GetSiblingOrder(),
            std::move(components.light),
            std::move(components.camera),
            std::move(components.rigidBody),
            std::move(components.collider));

        SceneObject& duplicatedObject = objects.back();
        if (!source.GetImportAssetPath().empty() && source.GetImportNodeIndex() >= 0)
        {
            duplicatedObject.SetImportSource(source.GetImportAssetPath(), source.GetImportNodeIndex());
        }

        scene.GetObjectStore().FinalizeNewObject(duplicatedObject);
        const int newIndex = static_cast<int>(objects.size()) - 1;
        indexMap[sourceIndex] = newIndex;
        if (sourceIndex == objectIndex)
        {
            duplicateRootIndex = newIndex;
        }
    }

    if (duplicateRootIndex < 0)
    {
        return -1;
    }

    scene.PlaceObjectInHierarchy(duplicateRootIndex, objectIndex, HierarchyInsertMode::After);
    return duplicateRootIndex;
}

std::vector<int> SceneObjectOperations::DuplicateSelectedObjects(Scene& scene)
{
    if (!scene.HasSelection())
    {
        return {};
    }

    std::vector<int> rootsToDuplicate =
        FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
    if (rootsToDuplicate.empty())
    {
        return {};
    }

    std::sort(rootsToDuplicate.begin(), rootsToDuplicate.end());

    std::vector<int> duplicatedIndices;
    duplicatedIndices.reserve(rootsToDuplicate.size());
    for (int objectIndex : rootsToDuplicate)
    {
        const int duplicatedIndex = DuplicateObject(scene, objectIndex);
        if (duplicatedIndex >= 0)
        {
            duplicatedIndices.push_back(duplicatedIndex);
        }
    }

    if (!duplicatedIndices.empty())
    {
        scene.SetSelection(duplicatedIndices, duplicatedIndices.back());
    }

    return duplicatedIndices;
}
