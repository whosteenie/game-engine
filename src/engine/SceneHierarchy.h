#pragma once

#include <glm/glm.hpp>
#include <vector>

class SceneObject;

glm::mat4 GetObjectWorldMatrix(const std::vector<SceneObject>& objects, int objectIndex);
void GetObjectWorldBounds(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax);
std::vector<int> GetObjectChildren(const std::vector<SceneObject>& objects, int parentIndex);
std::vector<int> GetRootObjectIndices(const std::vector<SceneObject>& objects);
