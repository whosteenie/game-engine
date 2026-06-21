#include "engine/ScenePicker.h"

#include "engine/SceneHierarchy.h"
#include "engine/SceneObject.h"

#include <algorithm>
#include <limits>

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
    const glm::vec3 inverseDirection = 1.0f / ray.direction;
    const glm::vec3 t0 = (boundsMin - ray.origin) * inverseDirection;
    const glm::vec3 t1 = (boundsMax - ray.origin) * inverseDirection;

    const glm::vec3 tMin = glm::min(t0, t1);
    const glm::vec3 tMax = glm::max(t0, t1);

    const float nearDistance = std::max(std::max(tMin.x, tMin.y), tMin.z);
    const float farDistance = std::min(std::min(tMax.x, tMax.y), tMax.z);

    if (farDistance < 0.0f || nearDistance > farDistance)
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
        if (left.boundsVolume != right.boundsVolume)
        {
            return left.boundsVolume < right.boundsVolume;
        }

        return left.distance < right.distance;
    }
}

std::vector<PickHit> PickAllSceneObjects(const std::vector<SceneObject>& objects, const Ray& ray)
{
    std::vector<PickHit> hits;

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.IsRenderable())
        {
            continue;
        }

        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        GetObjectWorldBounds(objects, objectIndex, boundsMin, boundsMax);

        float hitDistance = 0.0f;
        if (!IntersectRayAabb(ray, boundsMin, boundsMax, hitDistance))
        {
            continue;
        }

        const glm::vec3 boundsSize = boundsMax - boundsMin;
        PickHit hit;
        hit.objectIndex = objectIndex;
        hit.distance = hitDistance;
        hit.boundsVolume = boundsSize.x * boundsSize.y * boundsSize.z;
        hits.push_back(hit);
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
