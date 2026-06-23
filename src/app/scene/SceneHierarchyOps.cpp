#include "app/scene/SceneHierarchyOps.h"

#include "app/scene/Scene.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/scene/SceneObject.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

void SceneHierarchyOps::CollectDescendantIndices(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    std::vector<int>& outIndices)
{
    outIndices.push_back(objectIndex);
    for (int childIndex : GetObjectChildren(objects, objectIndex))
    {
        CollectDescendantIndices(objects, childIndex, outIndices);
    }
}

void SceneHierarchyOps::RemapParentIndicesAfterRemoval(
    std::vector<SceneObject>& objects,
    int removedIndex)
{
    for (SceneObject& object : objects)
    {
        int parentIndex = object.GetParentIndex();
        if (parentIndex > removedIndex)
        {
            object.SetParentIndex(parentIndex - 1);
        }
    }
}

int SceneHierarchyOps::AllocateSiblingOrder(
    const std::vector<SceneObject>& objects,
    int parentIndex)
{
    int maxOrder = -1;
    for (const SceneObject& object : objects)
    {
        if (object.GetParentIndex() == parentIndex)
        {
            maxOrder = std::max(maxOrder, object.GetSiblingOrder());
        }
    }

    return maxOrder + 1;
}

void SceneHierarchyOps::SetSiblingIndexAmongParent(
    std::vector<SceneObject>& objects,
    int objectIndex,
    int parentIndex,
    int siblingIndex)
{
    std::vector<int> siblings = GetObjectChildren(objects, parentIndex);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    if (siblingIndex < 0)
    {
        siblingIndex = 0;
    }
    else if (siblingIndex > static_cast<int>(siblings.size()))
    {
        siblingIndex = static_cast<int>(siblings.size());
    }

    siblings.insert(siblings.begin() + siblingIndex, objectIndex);

    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        objects[static_cast<std::size_t>(siblings[static_cast<std::size_t>(index)])].SetSiblingOrder(index);
    }
}

bool SceneHierarchyOps::CanReparentObject(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    int newParentIndex)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(objects.size()))
    {
        return false;
    }

    if (newParentIndex < -1 || newParentIndex >= static_cast<int>(objects.size()))
    {
        return false;
    }

    if (objectIndex == newParentIndex)
    {
        return false;
    }

    if (newParentIndex >= 0 && IsObjectDescendantOf(objects, objectIndex, newParentIndex))
    {
        return false;
    }

    return true;
}

bool SceneHierarchyOps::ReparentObject(
    std::vector<SceneObject>& objects,
    int objectIndex,
    int newParentIndex,
    const std::function<glm::mat4(int)>& getWorldMatrix)
{
    if (!CanReparentObject(objects, objectIndex, newParentIndex))
    {
        return false;
    }

    SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
    if (object.GetParentIndex() == newParentIndex)
    {
        return true;
    }

    const glm::mat4 worldMatrix = getWorldMatrix(objectIndex);

    object.SetParentIndex(newParentIndex);
    SetObjectWorldMatrix(objects, objectIndex, worldMatrix);
    return true;
}

bool SceneHierarchyOps::CanPlaceObjectInHierarchy(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(objects.size()))
    {
        return false;
    }

    if (referenceIndex < 0 || referenceIndex >= static_cast<int>(objects.size()))
    {
        return false;
    }

    if (objectIndex == referenceIndex)
    {
        return false;
    }

    if (IsObjectDescendantOf(objects, objectIndex, referenceIndex))
    {
        return false;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        return CanReparentObject(objects, objectIndex, referenceIndex);
    }

    const int referenceParent = objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    return CanReparentObject(objects, objectIndex, referenceParent);
}

bool SceneHierarchyOps::WouldPlaceObjectInHierarchyChange(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode)
{
    if (!CanPlaceObjectInHierarchy(objects, objectIndex, referenceIndex, mode))
    {
        return false;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        if (objects[static_cast<std::size_t>(objectIndex)].GetParentIndex() != referenceIndex)
        {
            return true;
        }

        const std::vector<int> children = GetObjectChildren(objects, referenceIndex);
        return children.empty() || children.back() != objectIndex;
    }

    const int targetParent = objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    const int currentParent = objects[static_cast<std::size_t>(objectIndex)].GetParentIndex();

    std::vector<int> siblings = GetObjectChildren(objects, targetParent);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    int targetSiblingIndex = static_cast<int>(siblings.size());
    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        if (siblings[static_cast<std::size_t>(index)] == referenceIndex)
        {
            targetSiblingIndex = index + (mode == HierarchyInsertMode::After ? 1 : 0);
            break;
        }
    }

    if (currentParent != targetParent)
    {
        return true;
    }

    const std::vector<int> siblingsWithSelf = GetObjectChildren(objects, targetParent);
    const auto currentIterator = std::find(siblingsWithSelf.begin(), siblingsWithSelf.end(), objectIndex);
    if (currentIterator == siblingsWithSelf.end())
    {
        return true;
    }

    const int currentSiblingIndex = static_cast<int>(currentIterator - siblingsWithSelf.begin());
    return currentSiblingIndex != targetSiblingIndex;
}

bool SceneHierarchyOps::PlaceObjectInHierarchy(
    std::vector<SceneObject>& objects,
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode,
    const std::function<glm::mat4(int)>& getWorldMatrix)
{
    if (!CanPlaceObjectInHierarchy(objects, objectIndex, referenceIndex, mode))
    {
        return false;
    }

    if (!WouldPlaceObjectInHierarchyChange(objects, objectIndex, referenceIndex, mode))
    {
        return true;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        if (!ReparentObject(objects, objectIndex, referenceIndex, getWorldMatrix))
        {
            return false;
        }

        const std::vector<int> children = GetObjectChildren(objects, referenceIndex);
        const int childIndex = static_cast<int>(
            std::find(children.begin(), children.end(), objectIndex) - children.begin());
        SetSiblingIndexAmongParent(objects, objectIndex, referenceIndex, childIndex);
        return true;
    }

    const int referenceParent = objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    if (!ReparentObject(objects, objectIndex, referenceParent, getWorldMatrix))
    {
        return false;
    }

    std::vector<int> siblings = GetObjectChildren(objects, referenceParent);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    int insertIndex = 0;
    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        if (siblings[static_cast<std::size_t>(index)] == referenceIndex)
        {
            insertIndex = index + (mode == HierarchyInsertMode::After ? 1 : 0);
            break;
        }
    }

    SetSiblingIndexAmongParent(objects, objectIndex, referenceParent, insertIndex);
    return true;
}

bool SceneHierarchyOps::PlaceObjectAtRootEnd(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const std::function<glm::mat4(int)>& getWorldMatrix)
{
    if (!CanReparentObject(objects, objectIndex, -1))
    {
        return false;
    }

    const std::vector<int> roots = GetRootObjectIndices(objects);
    if (roots.empty())
    {
        if (!ReparentObject(objects, objectIndex, -1, getWorldMatrix))
        {
            return false;
        }

        objects[static_cast<std::size_t>(objectIndex)].SetSiblingOrder(0);
        return true;
    }

    return PlaceObjectInHierarchy(
        objects,
        objectIndex,
        roots.back(),
        HierarchyInsertMode::After,
        getWorldMatrix);
}

bool SceneHierarchyOps::PlaceObjectAtRootBeginning(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const std::function<glm::mat4(int)>& getWorldMatrix)
{
    if (!CanReparentObject(objects, objectIndex, -1))
    {
        return false;
    }

    const std::vector<int> roots = GetRootObjectIndices(objects);
    if (roots.empty())
    {
        if (!ReparentObject(objects, objectIndex, -1, getWorldMatrix))
        {
            return false;
        }

        objects[static_cast<std::size_t>(objectIndex)].SetSiblingOrder(0);
        return true;
    }

    return PlaceObjectInHierarchy(
        objects,
        objectIndex,
        roots.front(),
        HierarchyInsertMode::Before,
        getWorldMatrix);
}
