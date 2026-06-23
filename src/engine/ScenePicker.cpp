#include "engine/ScenePicker.h"

#include "engine/Mesh.h"
#include "engine/SceneHierarchy.h"
#include "engine/SceneObject.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

Ray ScreenPointToRay(
    const glm::vec2& screenPoint,
    const glm::vec2& viewportSize,
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix)
{
    const float normalizedX = (2.0f * screenPoint.x) / viewportSize.x - 1.0f;
    const float normalizedY = 1.0f - (2.0f * screenPoint.y) / viewportSize.y;

    const glm::vec4 clipNear(normalizedX, normalizedY, -1.0f, 1.0f);
    const glm::vec4 clipFar(normalizedX, normalizedY, 1.0f, 1.0f);

    const glm::mat4 inverseViewProjection = glm::inverse(projectionMatrix * viewMatrix);
    glm::vec4 worldNear = inverseViewProjection * clipNear;
    glm::vec4 worldFar = inverseViewProjection * clipFar;
    worldNear /= worldNear.w;
    worldFar /= worldFar.w;

    Ray ray;
    ray.origin = glm::vec3(worldNear);
    ray.direction = glm::normalize(glm::vec3(worldFar - worldNear));
    return ray;
}

bool IntersectRayAabb(
    const Ray& ray,
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    float& hitDistance)
{
    constexpr float epsilon = 1e-7f;

    float nearDistance = 0.0f;
    float farDistance = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis)
    {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        float axisNear = 0.0f;
        float axisFar = 0.0f;

        if (std::abs(direction) < epsilon)
        {
            if (origin < boundsMin[axis] || origin > boundsMax[axis])
            {
                return false;
            }

            continue;
        }

        const float inverseDirection = 1.0f / direction;
        axisNear = (boundsMin[axis] - origin) * inverseDirection;
        axisFar = (boundsMax[axis] - origin) * inverseDirection;
        if (axisNear > axisFar)
        {
            std::swap(axisNear, axisFar);
        }

        nearDistance = std::max(nearDistance, axisNear);
        farDistance = std::min(farDistance, axisFar);

        if (nearDistance > farDistance)
        {
            return false;
        }
    }

    if (farDistance < 0.0f)
    {
        return false;
    }

    hitDistance = nearDistance >= 0.0f ? nearDistance : farDistance;
    return hitDistance >= 0.0f;
}

namespace
{
    bool ComparePickHits(const PickHit& left, const PickHit& right)
    {
        if (left.distance != right.distance)
        {
            return left.distance < right.distance;
        }

        return left.objectIndex < right.objectIndex;
    }

    int ResolvePickTarget(const std::vector<SceneObject>& objects, int objectIndex)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            return objectIndex;
        }

        int currentIndex = objectIndex;
        while (currentIndex >= 0)
        {
            const SceneObject& object = objects[static_cast<std::size_t>(currentIndex)];
            const int parentIndex = object.GetParentIndex();
            if (parentIndex < 0)
            {
                break;
            }

            const SceneObject& parent = objects[static_cast<std::size_t>(parentIndex)];
            if (parent.GetImportAssetPath().empty()
                || parent.GetImportAssetPath() != object.GetImportAssetPath())
            {
                break;
            }

            currentIndex = parentIndex;
        }

        return currentIndex;
    }

    bool IntersectSceneObjectMesh(
        const SceneObject& object,
        const glm::mat4& worldMatrix,
        const Ray& ray,
        float& hitDistance)
    {
        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr)
        {
            return false;
        }

        const glm::mat4 inverseWorldMatrix = glm::inverse(worldMatrix);
        const glm::vec3 localOrigin = glm::vec3(inverseWorldMatrix * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localDirection = glm::vec3(inverseWorldMatrix * glm::vec4(ray.direction, 0.0f));
        const float directionLength = glm::length(localDirection);
        if (directionLength < 1e-7f)
        {
            return false;
        }

        localDirection /= directionLength;
        if (!mesh->IntersectRay(localOrigin, localDirection, hitDistance))
        {
            return false;
        }

        const glm::vec3 localHit = localOrigin + localDirection * hitDistance;
        const glm::vec3 worldHit = glm::vec3(worldMatrix * glm::vec4(localHit, 1.0f));
        hitDistance = glm::length(worldHit - ray.origin);
        return true;
    }
}

std::vector<PickHit> PickAllSceneObjects(const std::vector<SceneObject>& objects, const Ray& ray)
{
    std::vector<PickHit> meshHits;

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.IsRenderable() || object.GetMesh() == nullptr)
        {
            continue;
        }

        const glm::mat4 worldMatrix = GetObjectWorldMatrix(objects, objectIndex);

        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        object.GetWorldBounds(worldMatrix, boundsMin, boundsMax);

        float boundsDistance = 0.0f;
        if (!IntersectRayAabb(ray, boundsMin, boundsMax, boundsDistance))
        {
            continue;
        }

        float meshDistance = 0.0f;
        if (!IntersectSceneObjectMesh(object, worldMatrix, ray, meshDistance))
        {
            continue;
        }

        const glm::vec3 boundsSize = boundsMax - boundsMin;
        PickHit hit;
        hit.objectIndex = objectIndex;
        hit.distance = meshDistance;
        hit.boundsVolume = boundsSize.x * boundsSize.y * boundsSize.z;
        meshHits.push_back(hit);
    }

    std::sort(meshHits.begin(), meshHits.end(), ComparePickHits);

    std::unordered_map<int, PickHit> closestByTarget;
    closestByTarget.reserve(meshHits.size());
    for (const PickHit& hit : meshHits)
    {
        const int targetIndex = ResolvePickTarget(objects, hit.objectIndex);
        PickHit resolvedHit = hit;
        resolvedHit.objectIndex = targetIndex;

        const auto existing = closestByTarget.find(targetIndex);
        if (existing == closestByTarget.end() || hit.distance < existing->second.distance)
        {
            closestByTarget[targetIndex] = resolvedHit;
        }
    }

    std::vector<PickHit> hits;
    hits.reserve(closestByTarget.size());
    for (const auto& entry : closestByTarget)
    {
        hits.push_back(entry.second);
    }

    std::sort(hits.begin(), hits.end(), ComparePickHits);
    return hits;
}

int PickSceneObjectCycling(
    const std::vector<SceneObject>& objects,
    const Ray& ray,
    int currentSelection,
    bool repeatClickAtSameSpot)
{
    const std::vector<PickHit> hits = PickAllSceneObjects(objects, ray);
    if (hits.empty())
    {
        return -1;
    }

    if (repeatClickAtSameSpot && currentSelection >= 0)
    {
        for (std::size_t hitIndex = 0; hitIndex < hits.size(); ++hitIndex)
        {
            if (hits[hitIndex].objectIndex == currentSelection)
            {
                const std::size_t nextHitIndex = (hitIndex + 1) % hits.size();
                return hits[nextHitIndex].objectIndex;
            }
        }
    }

    return hits.front().objectIndex;
}

int PickSceneObject(const std::vector<SceneObject>& objects, const Ray& ray)
{
    const std::vector<PickHit> hits = PickAllSceneObjects(objects, ray);
    if (hits.empty())
    {
        return -1;
    }

    return hits.front().objectIndex;
}
