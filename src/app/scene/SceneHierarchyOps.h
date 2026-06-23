#pragma once

#include <functional>
#include <glm/glm.hpp>
#include <vector>

class SceneObject;
enum class HierarchyInsertMode;

class SceneHierarchyOps
{
public:
    static void CollectDescendantIndices(
        const std::vector<SceneObject>& objects,
        int objectIndex,
        std::vector<int>& outIndices);

    static void RemapParentIndicesAfterRemoval(
        std::vector<SceneObject>& objects,
        int removedIndex);

    static int AllocateSiblingOrder(
        const std::vector<SceneObject>& objects,
        int parentIndex);

    static void SetSiblingIndexAmongParent(
        std::vector<SceneObject>& objects,
        int objectIndex,
        int parentIndex,
        int siblingIndex);

    static bool CanReparentObject(
        const std::vector<SceneObject>& objects,
        int objectIndex,
        int newParentIndex);

    static bool ReparentObject(
        std::vector<SceneObject>& objects,
        int objectIndex,
        int newParentIndex,
        const std::function<glm::mat4(int)>& getWorldMatrix);

    static bool CanPlaceObjectInHierarchy(
        const std::vector<SceneObject>& objects,
        int objectIndex,
        int referenceIndex,
        HierarchyInsertMode mode);

    static bool WouldPlaceObjectInHierarchyChange(
        const std::vector<SceneObject>& objects,
        int objectIndex,
        int referenceIndex,
        HierarchyInsertMode mode);

    static bool PlaceObjectInHierarchy(
        std::vector<SceneObject>& objects,
        int objectIndex,
        int referenceIndex,
        HierarchyInsertMode mode,
        const std::function<glm::mat4(int)>& getWorldMatrix);

    static bool PlaceObjectAtRootEnd(
        std::vector<SceneObject>& objects,
        int objectIndex,
        const std::function<glm::mat4(int)>& getWorldMatrix);

    static bool PlaceObjectAtRootBeginning(
        std::vector<SceneObject>& objects,
        int objectIndex,
        const std::function<glm::mat4(int)>& getWorldMatrix);
};
