#include <glm/gtc/matrix_transform.hpp>

#include "engine/scene/SceneHierarchy.h"

#include "engine/rendering/Mesh.h"
#include "engine/scene/SceneObject.h"

#include <algorithm>

#include <array>
#include <limits>
#include <unordered_set>

namespace
{
    void ExpandBounds(glm::vec3& boundsMin, glm::vec3& boundsMax, const glm::vec3& point)
    {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }

    void ExpandBoundsWithOrientedBox(
        glm::vec3& boundsMin,
        glm::vec3& boundsMax,
        const glm::mat4& matrix,
        const glm::vec3& localMin,
        const glm::vec3& localMax,
        bool& hasBounds)
    {
        const std::array<glm::vec3, 8> corners = {
            glm::vec3(localMin.x, localMin.y, localMin.z),
            glm::vec3(localMax.x, localMin.y, localMin.z),
            glm::vec3(localMin.x, localMax.y, localMin.z),
            glm::vec3(localMax.x, localMax.y, localMin.z),
            glm::vec3(localMin.x, localMin.y, localMax.z),
            glm::vec3(localMax.x, localMin.y, localMax.z),
            glm::vec3(localMin.x, localMax.y, localMax.z),
            glm::vec3(localMax.x, localMax.y, localMax.z),
        };

        for (const glm::vec3& corner : corners)
        {
            const glm::vec3 transformed = glm::vec3(matrix * glm::vec4(corner, 1.0f));
            if (!hasBounds)
            {
                boundsMin = transformed;
                boundsMax = transformed;
                hasBounds = true;
            }
            else
            {
                ExpandBounds(boundsMin, boundsMax, transformed);
            }
        }
    }
}

glm::mat4 GetObjectWorldMatrix(const std::vector<SceneObject>& objects, int objectIndex)
{
    const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
    const glm::mat4 localMatrix = object.GetTransform().ToMatrix();
    const int parentIndex = object.GetParentIndex();
    if (parentIndex < 0)
    {
        return localMatrix;
    }

    return GetObjectWorldMatrix(objects, parentIndex) * localMatrix;
}

void SetObjectWorldMatrix(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const glm::mat4& worldMatrix)
{
    if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
    {
        return;
    }

    glm::mat4 parentWorldMatrix(1.0f);
    const int parentIndex = objects[static_cast<std::size_t>(objectIndex)].GetParentIndex();
    if (parentIndex >= 0)
    {
        parentWorldMatrix = GetObjectWorldMatrix(objects, parentIndex);
    }

    objects[static_cast<std::size_t>(objectIndex)].GetTransform().SetFromMatrix(
        glm::inverse(parentWorldMatrix) * worldMatrix);
}

void GetObjectLocalSelectionBounds(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax)
{
    const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
    bool hasBounds = false;
    boundsMin = glm::vec3(std::numeric_limits<float>::max());
    boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    if (object.IsRenderable())
    {
        ExpandBoundsWithOrientedBox(
            boundsMin,
            boundsMax,
            glm::mat4(1.0f),
            object.GetLocalBoundsMin(),
            object.GetLocalBoundsMax(),
            hasBounds);
    }

    const glm::mat4 worldToLocal = glm::inverse(GetObjectWorldMatrix(objects, objectIndex));
    for (int childIndex : GetObjectChildren(objects, objectIndex))
    {
        glm::vec3 childBoundsMin;
        glm::vec3 childBoundsMax;
        GetObjectLocalSelectionBounds(objects, childIndex, childBoundsMin, childBoundsMax);

        const glm::mat4 childToLocal = worldToLocal * GetObjectWorldMatrix(objects, childIndex);
        ExpandBoundsWithOrientedBox(
            boundsMin,
            boundsMax,
            childToLocal,
            childBoundsMin,
            childBoundsMax,
            hasBounds);
    }

    if (!hasBounds)
    {
        boundsMin = glm::vec3(-0.15f);
        boundsMax = glm::vec3(0.15f);
    }
}

void GetObjectWorldBounds(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax)
{
    glm::vec3 localBoundsMin;
    glm::vec3 localBoundsMax;
    GetObjectLocalSelectionBounds(objects, objectIndex, localBoundsMin, localBoundsMax);

    const glm::mat4 worldMatrix = GetObjectWorldMatrix(objects, objectIndex);
    bool hasBounds = false;
    boundsMin = glm::vec3(std::numeric_limits<float>::max());
    boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    ExpandBoundsWithOrientedBox(
        boundsMin,
        boundsMax,
        worldMatrix,
        localBoundsMin,
        localBoundsMax,
        hasBounds);
}

glm::mat4 GetObjectGizmoWorldMatrix(const std::vector<SceneObject>& objects, int objectIndex)
{
    glm::vec3 localBoundsMin;
    glm::vec3 localBoundsMax;
    GetObjectLocalSelectionBounds(objects, objectIndex, localBoundsMin, localBoundsMax);
    const glm::vec3 localCenter = (localBoundsMin + localBoundsMax) * 0.5f;
    return GetObjectWorldMatrix(objects, objectIndex) * glm::translate(glm::mat4(1.0f), localCenter);
}

void ApplyObjectGizmoWorldMatrix(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const glm::mat4& gizmoWorldMatrix)
{
    glm::vec3 localBoundsMin;
    glm::vec3 localBoundsMax;
    GetObjectLocalSelectionBounds(objects, objectIndex, localBoundsMin, localBoundsMax);
    const glm::vec3 localCenter = (localBoundsMin + localBoundsMax) * 0.5f;

    const glm::mat4 newWorldMatrix =
        gizmoWorldMatrix * glm::translate(glm::mat4(1.0f), -localCenter);

    SetObjectWorldMatrix(objects, objectIndex, newWorldMatrix);
}

glm::mat4 GetGroupSelectionGizmoWorldMatrix(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    int primaryIndex,
    bool worldSpace)
{
    glm::vec3 boundsMin = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool hasBounds = false;

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        glm::vec3 objectBoundsMin;
        glm::vec3 objectBoundsMax;
        GetObjectWorldBounds(objects, objectIndex, objectBoundsMin, objectBoundsMax);

        if (!hasBounds)
        {
            boundsMin = objectBoundsMin;
            boundsMax = objectBoundsMax;
            hasBounds = true;
        }
        else
        {
            boundsMin = glm::min(boundsMin, objectBoundsMin);
            boundsMax = glm::max(boundsMax, objectBoundsMax);
        }
    }

    if (!hasBounds)
    {
        return glm::mat4(1.0f);
    }

    const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    if (worldSpace)
    {
        return glm::translate(glm::mat4(1.0f), center);
    }

    int orientationIndex = primaryIndex;
    if (orientationIndex < 0
        || std::find(objectIndices.begin(), objectIndices.end(), orientationIndex) == objectIndices.end())
    {
        orientationIndex = objectIndices.front();
    }

    glm::mat4 gizmoWorldMatrix = GetObjectWorldMatrix(objects, orientationIndex);
    gizmoWorldMatrix[3] = glm::vec4(center, 1.0f);
    return gizmoWorldMatrix;
}

void ApplyGroupSelectionGizmoWorldMatrix(
    std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    const glm::mat4& oldGizmoWorldMatrix,
    const glm::mat4& newGizmoWorldMatrix)
{
    const glm::mat4 deltaMatrix = newGizmoWorldMatrix * glm::inverse(oldGizmoWorldMatrix);

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];

        const glm::mat4 oldWorldMatrix = GetObjectWorldMatrix(objects, objectIndex);
        const glm::mat4 newWorldMatrix = deltaMatrix * oldWorldMatrix;
        SetObjectWorldMatrix(objects, objectIndex, newWorldMatrix);
    }
}

std::vector<int> GetObjectChildren(const std::vector<SceneObject>& objects, int parentIndex)
{
    std::vector<int> children;
    for (int index = 0; index < static_cast<int>(objects.size()); ++index)
    {
        if (objects[static_cast<std::size_t>(index)].GetParentIndex() == parentIndex)
        {
            children.push_back(index);
        }
    }

    std::sort(
        children.begin(),
        children.end(),
        [&objects](int leftIndex, int rightIndex) {
            return objects[static_cast<std::size_t>(leftIndex)].GetSiblingOrder()
                < objects[static_cast<std::size_t>(rightIndex)].GetSiblingOrder();
        });

    return children;
}

void CollectRenderableSelectionMeshes(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    std::vector<SelectionMeshDraw>& outMeshes)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(objects.size()))
    {
        return;
    }

    const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
    if (object.IsRenderable() && object.HasMesh())
    {
        outMeshes.push_back(SelectionMeshDraw{object.GetMesh(), GetObjectWorldMatrix(objects, objectIndex)});
    }

    for (int childIndex : GetObjectChildren(objects, objectIndex))
    {
        CollectRenderableSelectionMeshes(objects, childIndex, outMeshes);
    }
}

void CollectSelectionMeshes(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    std::vector<SelectionMeshDraw>& outMeshes)
{
    for (int objectIndex : objectIndices)
    {
        CollectRenderableSelectionMeshes(objects, objectIndex, outMeshes);
    }
}

std::vector<int> GetRootObjectIndices(const std::vector<SceneObject>& objects)
{
    return GetObjectChildren(objects, -1);
}

bool IsObjectDescendantOf(const std::vector<SceneObject>& objects, int ancestor, int objectIndex)
{
    int current = objectIndex;
    while (current >= 0)
    {
        if (current == ancestor)
        {
            return true;
        }

        current = objects[static_cast<std::size_t>(current)].GetParentIndex();
    }

    return false;
}

std::vector<int> FilterToTopmostSelectedIndices(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& selectedIndices)
{
    std::vector<int> topmostIndices;
    topmostIndices.reserve(selectedIndices.size());

    const std::unordered_set<int> selectedSet(selectedIndices.begin(), selectedIndices.end());

    for (int objectIndex : selectedIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        bool hasSelectedAncestor = false;
        int parentIndex = objects[static_cast<std::size_t>(objectIndex)].GetParentIndex();
        while (parentIndex >= 0)
        {
            if (selectedSet.find(parentIndex) != selectedSet.end())
            {
                hasSelectedAncestor = true;
                break;
            }

            parentIndex = objects[static_cast<std::size_t>(parentIndex)].GetParentIndex();
        }

        if (!hasSelectedAncestor)
        {
            topmostIndices.push_back(objectIndex);
        }
    }

    return topmostIndices;
}
