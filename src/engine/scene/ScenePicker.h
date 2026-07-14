#pragma once

#include <glm/glm.hpp>
#include <vector>

struct Ray
{
    glm::vec3 origin;
    glm::vec3 direction;
};

class SceneObject;

struct PickHit
{
    int objectIndex = -1;
    float distance = 0.0f;
    float boundsVolume = 0.0f;
};

struct SurfaceHit
{
    int objectIndex = -1;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

Ray ScreenPointToRay(
    const glm::vec2& screenPoint,
    const glm::vec2& viewportSize,
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix);

bool IntersectRayAabb(
    const Ray& ray,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    float& hitDistance);

int PickSceneObject(
    const std::vector<SceneObject>& objects,
    const Ray& ray);

std::vector<PickHit> PickAllSceneObjects(
    const std::vector<SceneObject>& objects,
    const Ray& ray);

int PickSceneObjectCycling(
    const std::vector<SceneObject>& objects,
    const Ray& ray,
    int currentSelection,
    bool repeatClickAtSameSpot);

std::vector<int> PickObjectsInScreenRect(
    const std::vector<SceneObject>& objects,
    const glm::vec2& rectMin,
    const glm::vec2& rectMax,
    const glm::vec2& viewportSize,
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix);

// Closest mesh hit under the ray. Skips objects whose own index or any ancestor is
// listed in excludeObjectIndices (used so a dragged selection does not snap to itself).
bool RaycastClosestSurface(
    const std::vector<SceneObject>& objects,
    const Ray& ray,
    const std::vector<int>& excludeObjectIndices,
    SurfaceHit& outHit);
