#include "engine/SceneHierarchy.h"

#include "engine/SceneObject.h"

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

void GetObjectWorldBounds(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax)
{
    const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
    object.GetWorldBounds(GetObjectWorldMatrix(objects, objectIndex), boundsMin, boundsMax);
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
