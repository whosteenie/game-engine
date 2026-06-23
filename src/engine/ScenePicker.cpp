#include "engine/ScenePicker.h"

#include "engine/Mesh.h"
#include "engine/SceneHierarchy.h"
#include "engine/SceneObject.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <array>
#include <unordered_map>
#include <unordered_set>

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

    bool TryWorldToScreenPoint(
        const glm::vec3& worldPoint,
        const glm::mat4& viewMatrix,
        const glm::mat4& projectionMatrix,
        const glm::vec2& viewportSize,
        glm::vec2& outScreenPoint)
    {
        const glm::vec4 clipSpace = projectionMatrix * viewMatrix * glm::vec4(worldPoint, 1.0f);
        if (clipSpace.w <= 1e-5f)
        {
            return false;
        }

        const glm::vec3 normalizedDeviceCoordinates = glm::vec3(clipSpace) / clipSpace.w;
        constexpr float kFrustumMargin = 1.001f;
        if (std::abs(normalizedDeviceCoordinates.x) > kFrustumMargin
            || std::abs(normalizedDeviceCoordinates.y) > kFrustumMargin
            || normalizedDeviceCoordinates.z < -1.0f || normalizedDeviceCoordinates.z > 1.0f)
        {
            return false;
        }

        outScreenPoint = glm::vec2(
            (normalizedDeviceCoordinates.x * 0.5f + 0.5f) * viewportSize.x,
            (1.0f - (normalizedDeviceCoordinates.y * 0.5f + 0.5f)) * viewportSize.y);
        return true;
    }

    bool IsPointInScreenRect(
        float screenX,
        float screenY,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY)
    {
        return screenX >= rectMinX && screenX <= rectMaxX && screenY >= rectMinY && screenY <= rectMaxY;
    }

    bool ScreenRectsOverlap(
        float aMinX,
        float aMinY,
        float aMaxX,
        float aMaxY,
        float bMinX,
        float bMinY,
        float bMaxX,
        float bMaxY)
    {
        return aMinX <= bMaxX && aMaxX >= bMinX && aMinY <= bMaxY && aMaxY >= bMinY;
    }

    bool WorldPointProjectsIntoScreenRect(
        const glm::vec3& worldPoint,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY,
        const glm::mat4& viewMatrix,
        const glm::mat4& projectionMatrix,
        const glm::vec2& viewportSize)
    {
        glm::vec2 screenPoint;
        if (!TryWorldToScreenPoint(
                worldPoint,
                viewMatrix,
                projectionMatrix,
                viewportSize,
                screenPoint))
        {
            return false;
        }

        return IsPointInScreenRect(
            screenPoint.x,
            screenPoint.y,
            rectMinX,
            rectMinY,
            rectMaxX,
            rectMaxY);
    }

    bool MeshIntersectsScreenRect(
        const Mesh& mesh,
        const glm::mat4& worldMatrix,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY,
        const glm::mat4& viewMatrix,
        const glm::mat4& projectionMatrix,
        const glm::vec2& viewportSize)
    {
        for (const glm::vec3& localPosition : mesh.GetPositions())
        {
            const glm::vec3 worldPosition = glm::vec3(worldMatrix * glm::vec4(localPosition, 1.0f));
            if (WorldPointProjectsIntoScreenRect(
                    worldPosition,
                    rectMinX,
                    rectMinY,
                    rectMaxX,
                    rectMaxY,
                    viewMatrix,
                    projectionMatrix,
                    viewportSize))
            {
                return true;
            }
        }

        const std::vector<unsigned int>& indices = mesh.GetIndices();
        const std::vector<glm::vec3>& positions = mesh.GetPositions();
        for (std::size_t index = 0; index + 2 < indices.size(); index += 3)
        {
            const unsigned int i0 = indices[index];
            const unsigned int i1 = indices[index + 1];
            const unsigned int i2 = indices[index + 2];
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
            {
                continue;
            }

            const glm::vec3 localCentroid = (positions[i0] + positions[i1] + positions[i2]) / 3.0f;
            const glm::vec3 worldCentroid = glm::vec3(worldMatrix * glm::vec4(localCentroid, 1.0f));
            if (WorldPointProjectsIntoScreenRect(
                    worldCentroid,
                    rectMinX,
                    rectMinY,
                    rectMaxX,
                    rectMaxY,
                    viewMatrix,
                    projectionMatrix,
                    viewportSize))
            {
                return true;
            }
        }

        return false;
    }

    bool BoundsIntersectScreenRect(
        const glm::vec3& boundsMin,
        const glm::vec3& boundsMax,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY,
        const glm::mat4& viewMatrix,
        const glm::mat4& projectionMatrix,
        const glm::vec2& viewportSize)
    {
        float objectMinX = std::numeric_limits<float>::max();
        float objectMinY = std::numeric_limits<float>::max();
        float objectMaxX = std::numeric_limits<float>::lowest();
        float objectMaxY = std::numeric_limits<float>::lowest();
        bool hasVisiblePoint = false;

        const auto considerWorldPoint = [&](const glm::vec3& worldPoint) {
            glm::vec2 screenPoint;
            if (!TryWorldToScreenPoint(
                    worldPoint,
                    viewMatrix,
                    projectionMatrix,
                    viewportSize,
                    screenPoint))
            {
                return;
            }

            hasVisiblePoint = true;
            objectMinX = std::min(objectMinX, screenPoint.x);
            objectMinY = std::min(objectMinY, screenPoint.y);
            objectMaxX = std::max(objectMaxX, screenPoint.x);
            objectMaxY = std::max(objectMaxY, screenPoint.y);
        };

        considerWorldPoint((boundsMin + boundsMax) * 0.5f);

        const std::array<glm::vec3, 8> corners = {
            glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
        };

        for (const glm::vec3& corner : corners)
        {
            considerWorldPoint(corner);
        }

        if (!hasVisiblePoint)
        {
            return false;
        }

        return ScreenRectsOverlap(
            objectMinX,
            objectMinY,
            objectMaxX,
            objectMaxY,
            rectMinX,
            rectMinY,
            rectMaxX,
            rectMaxY);
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

std::vector<int> PickObjectsInScreenRect(
    const std::vector<SceneObject>& objects,
    const glm::vec2& rectMin,
    const glm::vec2& rectMax,
    const glm::vec2& viewportSize,
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix)
{
    const float selectionMinX = std::min(rectMin.x, rectMax.x);
    const float selectionMinY = std::min(rectMin.y, rectMax.y);
    const float selectionMaxX = std::max(rectMin.x, rectMax.x);
    const float selectionMaxY = std::max(rectMin.y, rectMax.y);

    std::unordered_set<int> matchedTargets;
    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.IsRenderable() || object.GetMesh() == nullptr)
        {
            continue;
        }

        const int targetIndex = ResolvePickTarget(objects, objectIndex);

        const glm::mat4 worldMatrix = GetObjectWorldMatrix(objects, objectIndex);
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        object.GetWorldBounds(worldMatrix, boundsMin, boundsMax);

        if (!BoundsIntersectScreenRect(
                boundsMin,
                boundsMax,
                selectionMinX,
                selectionMinY,
                selectionMaxX,
                selectionMaxY,
                viewMatrix,
                projectionMatrix,
                viewportSize))
        {
            continue;
        }

        if (!MeshIntersectsScreenRect(
                *object.GetMesh(),
                worldMatrix,
                selectionMinX,
                selectionMinY,
                selectionMaxX,
                selectionMaxY,
                viewMatrix,
                projectionMatrix,
                viewportSize))
        {
            continue;
        }

        matchedTargets.insert(targetIndex);
    }

    std::vector<int> pickedIndices(matchedTargets.begin(), matchedTargets.end());
    std::sort(pickedIndices.begin(), pickedIndices.end());
    return pickedIndices;
}
