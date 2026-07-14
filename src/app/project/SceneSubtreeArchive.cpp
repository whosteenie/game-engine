#include "app/project/SceneSubtreeArchive.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectStore.h"

#include "engine/rendering/Mesh.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectComponents.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

ArchivedSelectionState CaptureArchivedSelection(const Scene& scene)
{
    ArchivedSelectionState selection;
    selection.ids = scene.GetSelectionIds();

    const int primaryIndex = scene.GetPrimarySelection();
    if (primaryIndex >= 0 && primaryIndex < static_cast<int>(scene.GetObjects().size()))
    {
        selection.primary = scene.GetObjects()[static_cast<std::size_t>(primaryIndex)].GetId();
    }

    return selection;
}

void ApplyArchivedSelection(Scene& scene, const ArchivedSelectionState& selection)
{
    scene.SetSelectionByIds(selection.ids, selection.primary);
}

SceneHierarchyArchive CaptureHierarchyArchive(const Scene& scene)
{
    SceneHierarchyArchive archive;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    archive.states.reserve(objects.size());

    for (const SceneObject& object : objects)
    {
        SceneObjectId parentId = kInvalidSceneObjectId;
        const int parentIndex = object.GetParentIndex();
        if (parentIndex >= 0 && parentIndex < static_cast<int>(objects.size()))
        {
            parentId = objects[static_cast<std::size_t>(parentIndex)].GetId();
        }

        archive.states.emplace(
            object.GetId(),
            ArchivedObjectHierarchy{parentId, object.GetSiblingOrder(), object.GetTransform()});
    }

    return archive;
}

bool AreHierarchyArchivesEqual(const SceneHierarchyArchive& left, const SceneHierarchyArchive& right)
{
    if (left.states.size() != right.states.size())
    {
        return false;
    }

    for (const auto& [objectId, state] : left.states)
    {
        const auto iterator = right.states.find(objectId);
        if (iterator == right.states.end())
        {
            return false;
        }

        const ArchivedObjectHierarchy& other = iterator->second;
        if (state.parentId != other.parentId || state.siblingOrder != other.siblingOrder
            || state.transform.position != other.transform.position
            || state.transform.rotation != other.transform.rotation
            || state.transform.scale != other.transform.scale)
        {
            return false;
        }
    }

    return true;
}

void ApplyHierarchyArchive(Scene& scene, const SceneHierarchyArchive& archive)
{
    std::vector<SceneObject>& objects = scene.GetObjects();
    for (SceneObject& object : objects)
    {
        const auto iterator = archive.states.find(object.GetId());
        if (iterator == archive.states.end())
        {
            continue;
        }

        const ArchivedObjectHierarchy& state = iterator->second;
        const int parentIndex =
            state.parentId == kInvalidSceneObjectId ? -1 : scene.FindObjectIndex(state.parentId);
        object.SetParentIndex(parentIndex);
        object.SetSiblingOrder(state.siblingOrder);
        object.GetTransform() = state.transform;
    }

    scene.MarkDirty();
}

namespace
{
    void CaptureParentIds(const Scene& scene, std::unordered_map<SceneObjectId, SceneObjectId>& outParentIdByObjectId)
    {
        outParentIdByObjectId.clear();
        const std::vector<SceneObject>& objects = scene.GetObjects();
        outParentIdByObjectId.reserve(objects.size());

        for (const SceneObject& object : objects)
        {
            SceneObjectId parentId = kInvalidSceneObjectId;
            const int parentIndex = object.GetParentIndex();
            if (parentIndex >= 0 && parentIndex < static_cast<int>(objects.size()))
            {
                parentId = objects[static_cast<std::size_t>(parentIndex)].GetId();
            }

            outParentIdByObjectId.emplace(object.GetId(), parentId);
        }
    }

    ArchivedSceneObject ArchiveObject(const Scene& scene, int objectIndex)
    {
        const SceneObject& source = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        SceneObjectComponentSnapshot components = CaptureSceneObjectComponents(source);

        ArchivedSceneObject archived;
        archived.flatIndex = objectIndex;
        archived.id = source.GetId();
        archived.name = source.GetName();
        archived.mesh = source.GetMesh();
        archived.localBoundsMin = source.GetLocalBoundsMin();
        archived.localBoundsMax = source.GetLocalBoundsMax();
        archived.transform = source.GetTransform();
        archived.castShadow = source.CastsShadow();
        archived.receiveShadow = source.ReceivesShadow();
        archived.siblingOrder = source.GetSiblingOrder();
        archived.material = std::move(components.material);
        archived.light = std::move(components.light);
        archived.camera = std::move(components.camera);
        archived.rigidBody = std::move(components.rigidBody);
        archived.collider = std::move(components.collider);
        archived.inspectorComponentOrder = std::move(components.inspectorComponentOrder);

        if (!source.GetImportAssetPath().empty() && source.GetImportNodeIndex() >= 0)
        {
            archived.isImportedMesh = true;
            archived.importedMeshKey = ImportMeshKey{source.GetImportAssetPath(), source.GetImportNodeIndex()};
        }

        return archived;
    }

    SceneObject BuildRestoredObject(
        Scene& scene,
        ArchivedSceneObject& archived,
        SceneSubtreeArchive& archive,
        std::unordered_map<ImportMeshKey, Mesh*, ImportMeshKeyHash>& restoredImportedMeshes)
    {
        Mesh* mesh = archived.mesh;
        if (archived.isImportedMesh)
        {
            const auto restoredIterator = restoredImportedMeshes.find(archived.importedMeshKey);
            if (restoredIterator != restoredImportedMeshes.end())
            {
                mesh = restoredIterator->second;
            }
            else
            {
                const auto poolIterator = archive.importedMeshes.find(archived.importedMeshKey);
                if (poolIterator != archive.importedMeshes.end())
                {
                    mesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(poolIterator->second));
                    archive.importedMeshes.erase(poolIterator);
                    restoredImportedMeshes.emplace(archived.importedMeshKey, mesh);
                }
            }
        }

        SceneObjectComponentSnapshot components;
        components.light = std::move(archived.light);
        components.camera = std::move(archived.camera);
        components.rigidBody = std::move(archived.rigidBody);
        components.collider = std::move(archived.collider);
        components.material = std::move(archived.material);

        SceneObject restored(
            archived.name,
            mesh,
            std::move(components.material),
            archived.localBoundsMin,
            archived.localBoundsMax,
            archived.transform,
            archived.castShadow,
            archived.receiveShadow,
            -1,
            archived.siblingOrder,
            std::move(components.light),
            std::move(components.camera),
            std::move(components.rigidBody),
            std::move(components.collider),
            archived.id);

        if (!archived.isImportedMesh)
        {
            restored.ClearImportSource();
        }
        else
        {
            restored.SetImportSource(
                archived.importedMeshKey.assetPath,
                archived.importedMeshKey.nodeIndex);
        }

        restored.SetInspectorComponentOrder(archived.inspectorComponentOrder);

        return restored;
    }
}

bool Scene::CreateDeleteArchive(
    const std::vector<int>& rootIndices,
    SceneSubtreeArchive& archive,
    bool transferImportedMeshOwnership)
{
    if (rootIndices.empty())
    {
        return false;
    }

    std::unordered_set<int> removalSet;
    for (int rootIndex : rootIndices)
    {
        std::vector<int> subtreeIndices;
        CollectDescendantIndices(rootIndex, subtreeIndices);
        for (int index : subtreeIndices)
        {
            removalSet.insert(index);
        }
    }

    std::vector<int> removalIndices(removalSet.begin(), removalSet.end());
    std::sort(removalIndices.begin(), removalIndices.end());
    if (removalIndices.empty())
    {
        return false;
    }

    archive.removedObjects.clear();
    archive.removedObjects.reserve(removalIndices.size());
    archive.removedRootIds.clear();
    archive.removedRootIds.reserve(rootIndices.size());
    archive.importedMeshes.clear();
    for (int rootIndex : rootIndices)
    {
        if (rootIndex < 0 || rootIndex >= static_cast<int>(GetObjects().size()))
        {
            continue;
        }

        archive.removedRootIds.push_back(GetObjects()[static_cast<std::size_t>(rootIndex)].GetId());
    }

    if (archive.removedRootIds.empty())
    {
        return false;
    }

    CaptureParentIds(*this, archive.parentIdByObjectId);

    std::unordered_set<Mesh*> externalMeshRefs;
    for (int objectIndex = 0; objectIndex < static_cast<int>(GetObjects().size()); ++objectIndex)
    {
        if (removalSet.find(objectIndex) != removalSet.end())
        {
            continue;
        }

        const SceneObject& object = GetObjects()[static_cast<std::size_t>(objectIndex)];
        if (object.HasMesh())
        {
            externalMeshRefs.insert(object.GetMesh());
        }
    }

    std::unordered_set<Mesh*> harvestedMeshes;
    for (int objectIndex : removalIndices)
    {
        ArchivedSceneObject archived = ArchiveObject(*this, objectIndex);
        if (transferImportedMeshOwnership && archived.isImportedMesh && archived.mesh != nullptr
            && externalMeshRefs.find(archived.mesh) == externalMeshRefs.end()
            && harvestedMeshes.find(archived.mesh) == harvestedMeshes.end())
        {
            std::unique_ptr<Mesh> ownedMesh = GetMeshLibrary().ExtractImportedMesh(archived.mesh);
            if (ownedMesh != nullptr)
            {
                archive.importedMeshes.try_emplace(archived.importedMeshKey, std::move(ownedMesh));
                harvestedMeshes.insert(archived.mesh);
                archived.ownsImportedMesh = true;
            }
        }

        archive.removedObjects.push_back(std::move(archived));
    }

    return true;
}

bool Scene::DeleteUsingArchive(const SceneSubtreeArchive& archive)
{
    if (archive.removedRootIds.empty())
    {
        return false;
    }

    std::vector<int> rootIndices;
    rootIndices.reserve(archive.removedRootIds.size());
    for (SceneObjectId rootId : archive.removedRootIds)
    {
        const int rootIndex = FindObjectIndex(rootId);
        if (rootIndex >= 0)
        {
            rootIndices.push_back(rootIndex);
        }
    }

    if (rootIndices.empty())
    {
        return false;
    }

    std::sort(rootIndices.begin(), rootIndices.end(), std::greater<int>());
    for (int rootIndex : rootIndices)
    {
        RemoveObject(static_cast<std::size_t>(rootIndex));
    }

    return true;
}

bool Scene::RestoreDeleteArchive(SceneSubtreeArchive& archive, const ArchivedSelectionState& selection)
{
    if (archive.removedObjects.empty())
    {
        return false;
    }

    std::vector<SceneObject> survivors;
    survivors.swap(GetObjects());

    const std::size_t totalCount = survivors.size() + archive.removedObjects.size();
    std::vector<SceneObject> merged;
    merged.reserve(totalCount);

    std::size_t survivorCursor = 0;
    std::size_t archiveCursor = 0;
    std::unordered_map<ImportMeshKey, Mesh*, ImportMeshKeyHash> restoredImportedMeshes;

    for (int flatIndex = 0; flatIndex < static_cast<int>(totalCount); ++flatIndex)
    {
        if (archiveCursor < archive.removedObjects.size()
            && archive.removedObjects[archiveCursor].flatIndex == flatIndex)
        {
            merged.push_back(BuildRestoredObject(
                *this,
                archive.removedObjects[archiveCursor],
                archive,
                restoredImportedMeshes));
            ++archiveCursor;
            continue;
        }

        if (survivorCursor >= survivors.size())
        {
            return false;
        }

        merged.push_back(std::move(survivors[survivorCursor]));
        ++survivorCursor;
    }

    if (archiveCursor != archive.removedObjects.size() || survivorCursor != survivors.size())
    {
        return false;
    }

    GetObjects() = std::move(merged);

    for (SceneObject& object : GetObjects())
    {
        const auto parentIterator = archive.parentIdByObjectId.find(object.GetId());
        if (parentIterator == archive.parentIdByObjectId.end())
        {
            continue;
        }

        const SceneObjectId parentId = parentIterator->second;
        const int parentIndex = parentId == kInvalidSceneObjectId ? -1 : FindObjectIndex(parentId);
        object.SetParentIndex(parentIndex);
        GetObjectStore().RegisterId(object.GetId());
    }

    ApplyArchivedSelection(*this, selection);
    NotifyRenderContentChanged();
    MarkDirty();
    return true;
}

SceneSubtreeArchive CloneSubtreeArchive(const SceneSubtreeArchive& source)
{
    SceneSubtreeArchive clone;
    clone.removedObjects.reserve(source.removedObjects.size());
    for (const ArchivedSceneObject& archivedObject : source.removedObjects)
    {
        ArchivedSceneObject copy;
        copy.flatIndex = archivedObject.flatIndex;
        copy.id = archivedObject.id;
        copy.name = archivedObject.name;
        copy.mesh = archivedObject.mesh;
        copy.importedMeshKey = archivedObject.importedMeshKey;
        copy.isImportedMesh = archivedObject.isImportedMesh;
        copy.ownsImportedMesh = false;
        copy.localBoundsMin = archivedObject.localBoundsMin;
        copy.localBoundsMax = archivedObject.localBoundsMax;
        copy.transform = archivedObject.transform;
        copy.castShadow = archivedObject.castShadow;
        copy.receiveShadow = archivedObject.receiveShadow;
        copy.siblingOrder = archivedObject.siblingOrder;
        if (archivedObject.material)
        {
            copy.material = archivedObject.material->Clone();
        }

        if (archivedObject.light.has_value())
        {
            copy.light = archivedObject.light;
        }

        if (archivedObject.camera.has_value())
        {
            copy.camera = archivedObject.camera;
        }

        if (archivedObject.rigidBody.has_value())
        {
            copy.rigidBody = archivedObject.rigidBody;
        }

        if (archivedObject.collider.has_value())
        {
            copy.collider = archivedObject.collider;
        }

        clone.removedObjects.push_back(std::move(copy));
    }

    clone.parentIdByObjectId = source.parentIdByObjectId;
    clone.removedRootIds = source.removedRootIds;
    return clone;
}

void RemapSubtreeArchiveIds(Scene& scene, SceneSubtreeArchive& archive)
{
    std::unordered_map<SceneObjectId, SceneObjectId> idMap;
    idMap.reserve(archive.removedObjects.size());

    for (ArchivedSceneObject& archivedObject : archive.removedObjects)
    {
        const SceneObjectId oldId = archivedObject.id;
        const SceneObjectId newId = scene.GetObjectStore().AllocateId();
        archivedObject.id = newId;
        idMap.emplace(oldId, newId);
    }

    for (SceneObjectId& rootId : archive.removedRootIds)
    {
        const auto iterator = idMap.find(rootId);
        if (iterator != idMap.end())
        {
            rootId = iterator->second;
        }
    }

    std::unordered_map<SceneObjectId, SceneObjectId> remappedParents;
    remappedParents.reserve(archive.parentIdByObjectId.size());
    for (const auto& [objectId, parentId] : archive.parentIdByObjectId)
    {
        const auto objectIterator = idMap.find(objectId);
        if (objectIterator == idMap.end())
        {
            continue;
        }

        SceneObjectId remappedParentId = kInvalidSceneObjectId;
        if (parentId != kInvalidSceneObjectId)
        {
            const auto parentIterator = idMap.find(parentId);
            if (parentIterator != idMap.end())
            {
                remappedParentId = parentIterator->second;
            }
        }

        remappedParents.emplace(objectIterator->second, remappedParentId);
    }

    archive.parentIdByObjectId = std::move(remappedParents);
}

std::vector<int> Scene::InsertSubtreeArchive(
    SceneSubtreeArchive& archive,
    int referenceIndex,
    HierarchyInsertMode rootPlacement)
{
    if (archive.removedObjects.empty() || archive.removedRootIds.empty())
    {
        return {};
    }

    std::unordered_set<SceneObjectId> archivedIds;
    archivedIds.reserve(archive.removedObjects.size());
    for (const ArchivedSceneObject& archivedObject : archive.removedObjects)
    {
        archivedIds.insert(archivedObject.id);
    }

    std::unordered_map<SceneObjectId, int> newIndexById;
    newIndexById.reserve(archive.removedObjects.size());
    std::unordered_map<ImportMeshKey, Mesh*, ImportMeshKeyHash> restoredImportedMeshes;

    for (ArchivedSceneObject& archivedObject : archive.removedObjects)
    {
        GetObjects().push_back(BuildRestoredObject(*this, archivedObject, archive, restoredImportedMeshes));
        SceneObject& object = GetObjects().back();
        GetObjectStore().FinalizeNewObject(object);
        newIndexById.emplace(object.GetId(), static_cast<int>(GetObjects().size()) - 1);
    }

    for (const ArchivedSceneObject& archivedObject : archive.removedObjects)
    {
        const auto objectIterator = newIndexById.find(archivedObject.id);
        if (objectIterator == newIndexById.end())
        {
            continue;
        }

        SceneObject& object = GetObjects()[static_cast<std::size_t>(objectIterator->second)];
        const auto parentIterator = archive.parentIdByObjectId.find(archivedObject.id);
        if (parentIterator == archive.parentIdByObjectId.end())
        {
            continue;
        }

        const SceneObjectId parentId = parentIterator->second;
        if (archivedIds.find(parentId) != archivedIds.end())
        {
            const auto parentIndexIterator = newIndexById.find(parentId);
            if (parentIndexIterator != newIndexById.end())
            {
                object.SetParentIndex(parentIndexIterator->second);
            }
        }
        else
        {
            object.SetParentIndex(-1);
        }
    }

    std::vector<int> insertedRootIndices;
    insertedRootIndices.reserve(archive.removedRootIds.size());

    int chainReference = referenceIndex;
    bool placedFirstRoot = false;
    for (SceneObjectId rootId : archive.removedRootIds)
    {
        const auto rootIterator = newIndexById.find(rootId);
        if (rootIterator == newIndexById.end())
        {
            continue;
        }

        const int rootIndex = rootIterator->second;
        insertedRootIndices.push_back(rootIndex);

        if (referenceIndex < 0)
        {
            PlaceObjectAtRootEnd(rootIndex);
            continue;
        }

        if (!placedFirstRoot)
        {
            PlaceObjectInHierarchy(rootIndex, referenceIndex, rootPlacement);
            chainReference = rootIndex;
            placedFirstRoot = true;
            continue;
        }

        PlaceObjectInHierarchy(rootIndex, chainReference, HierarchyInsertMode::After);
        chainReference = rootIndex;
    }

    if (!insertedRootIndices.empty())
    {
        MarkDirty();
    }

    return insertedRootIndices;
}
