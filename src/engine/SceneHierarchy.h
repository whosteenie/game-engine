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
void GetObjectLocalSelectionBounds(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    glm::vec3& boundsMin,
    glm::vec3& boundsMax);
glm::mat4 GetObjectGizmoWorldMatrix(const std::vector<SceneObject>& objects, int objectIndex);
void ApplyObjectGizmoWorldMatrix(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const glm::mat4& gizmoWorldMatrix);
std::vector<int> GetObjectChildren(const std::vector<SceneObject>& objects, int parentIndex);
std::vector<int> GetRootObjectIndices(const std::vector<SceneObject>& objects);
bool IsObjectDescendantOf(const std::vector<SceneObject>& objects, int ancestor, int objectIndex);
