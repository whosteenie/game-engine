#include <glm/gtc/matrix_transform.hpp>

#include "engine/SceneHierarchy.h"

#include "engine/SceneObject.h"

#include <array>
#include <limits>

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

    glm::mat4 parentWorldMatrix(1.0f);
    const int parentIndex = objects[static_cast<std::size_t>(objectIndex)].GetParentIndex();
    if (parentIndex >= 0)
    {
        parentWorldMatrix = GetObjectWorldMatrix(objects, parentIndex);
    }

    objects[static_cast<std::size_t>(objectIndex)].GetTransform().SetFromMatrix(
        glm::inverse(parentWorldMatrix) * newWorldMatrix);
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

    return children;
}

std::vector<int> GetRootObjectIndices(const std::vector<SceneObject>& objects)
{
    std::vector<int> roots;
    for (int index = 0; index < static_cast<int>(objects.size()); ++index)
    {
        if (objects[static_cast<std::size_t>(index)].GetParentIndex() < 0)
        {
            roots.push_back(index);
        }
    }

    return roots;
}
