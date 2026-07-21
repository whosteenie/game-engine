#pragma once

#include <glm/glm.hpp>
#include <vector>

class Mesh;
class SceneObject;

struct SelectionMeshDraw
{
    const Mesh* mesh = nullptr;
    glm::mat4 worldMatrix = glm::mat4(1.0f);
    glm::vec3 localBoundsMin{0.0f};
    glm::vec3 localBoundsMax{0.0f};
};

void CollectRenderableSelectionMeshes(
    const std::vector<SceneObject>& objects,
    int objectIndex,
    std::vector<SelectionMeshDraw>& outMeshes);
void CollectSelectionMeshes(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    std::vector<SelectionMeshDraw>& outMeshes);
glm::mat4 GetObjectWorldMatrix(const std::vector<SceneObject>& objects, int objectIndex);
void SetObjectWorldMatrix(
    std::vector<SceneObject>& objects,
    int objectIndex,
    const glm::mat4& worldMatrix);
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
    const glm::mat4& oldGizmoWorldMatrix,
    const glm::mat4& newGizmoWorldMatrix);
glm::mat4 GetGroupSelectionGizmoWorldMatrix(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    int primaryIndex,
    bool worldSpace);
void ApplyGroupSelectionGizmoWorldMatrix(
    std::vector<SceneObject>& objects,
    const std::vector<int>& objectIndices,
    const glm::mat4& oldGizmoWorldMatrix,
    const glm::mat4& newGizmoWorldMatrix);
std::vector<int> GetObjectChildren(const std::vector<SceneObject>& objects, int parentIndex);
std::vector<int> GetRootObjectIndices(const std::vector<SceneObject>& objects);
bool IsObjectDescendantOf(const std::vector<SceneObject>& objects, int ancestor, int objectIndex);
std::vector<int> FilterToTopmostSelectedIndices(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& selectedIndices);
