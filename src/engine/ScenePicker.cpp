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

    bool TryWorldToScreenPointRelaxed(
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
        if (normalizedDeviceCoordinates.z < -1.0f || normalizedDeviceCoordinates.z > 1.0f)
        {
            return false;
        }

        outScreenPoint = glm::vec2(
            (normalizedDeviceCoordinates.x * 0.5f + 0.5f) * viewportSize.x,
            (1.0f - (normalizedDeviceCoordinates.y * 0.5f + 0.5f)) * viewportSize.y);
        return true;
    }

    float Cross2D(const glm::vec2& left, const glm::vec2& right)
    {
        return left.x * right.y - left.y * right.x;
    }

    bool IsPointInTriangle2D(
        const glm::vec2& point,
        const glm::vec2& t0,
        const glm::vec2& t1,
        const glm::vec2& t2)
    {
        const float area = Cross2D(t1 - t0, t2 - t0);
        if (std::abs(area) < 1e-8f)
        {
            return false;
        }

        const float weight0 = Cross2D(t1 - point, t2 - point) / area;
        const float weight1 = Cross2D(t2 - point, t0 - point) / area;
        const float weight2 = 1.0f - weight0 - weight1;
        constexpr float epsilon = 1e-5f;
        return weight0 >= -epsilon && weight1 >= -epsilon && weight2 >= -epsilon;
    }

    bool SegmentsIntersect2D(
        const glm::vec2& segmentStartA,
        const glm::vec2& segmentEndA,
        const glm::vec2& segmentStartB,
        const glm::vec2& segmentEndB)
    {
        const auto orientation = [](const glm::vec2& p, const glm::vec2& q, const glm::vec2& r) {
            return Cross2D(q - p, r - p);
        };

        const auto onSegment = [](const glm::vec2& p, const glm::vec2& q, const glm::vec2& r) {
            constexpr float epsilon = 1e-5f;
            return q.x <= std::max(p.x, r.x) + epsilon && q.x >= std::min(p.x, r.x) - epsilon
                && q.y <= std::max(p.y, r.y) + epsilon && q.y >= std::min(p.y, r.y) - epsilon;
        };

        const float orientationAB_C = orientation(segmentStartA, segmentEndA, segmentStartB);
        const float orientationAB_D = orientation(segmentStartA, segmentEndA, segmentEndB);
        const float orientationCD_A = orientation(segmentStartB, segmentEndB, segmentStartA);
        const float orientationCD_B = orientation(segmentStartB, segmentEndB, segmentEndA);

        if (orientationAB_C * orientationAB_D < 0.0f && orientationCD_A * orientationCD_B < 0.0f)
        {
            return true;
        }

        constexpr float epsilon = 1e-5f;
        if (std::abs(orientationAB_C) < epsilon && onSegment(segmentStartA, segmentStartB, segmentEndA))
        {
            return true;
        }
        if (std::abs(orientationAB_D) < epsilon && onSegment(segmentStartA, segmentEndB, segmentEndA))
        {
            return true;
        }
        if (std::abs(orientationCD_A) < epsilon && onSegment(segmentStartB, segmentStartA, segmentEndB))
        {
            return true;
        }
        if (std::abs(orientationCD_B) < epsilon && onSegment(segmentStartB, segmentEndA, segmentEndB))
        {
            return true;
        }

        return false;
    }

    bool SegmentOverlapsScreenRect(
        const glm::vec2& segmentStart,
        const glm::vec2& segmentEnd,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY)
    {
        if (IsPointInScreenRect(segmentStart.x, segmentStart.y, rectMinX, rectMinY, rectMaxX, rectMaxY)
            || IsPointInScreenRect(segmentEnd.x, segmentEnd.y, rectMinX, rectMinY, rectMaxX, rectMaxY))
        {
            return true;
        }

        const std::array<glm::vec2, 4> rectCorners = {
            glm::vec2(rectMinX, rectMinY),
            glm::vec2(rectMaxX, rectMinY),
            glm::vec2(rectMaxX, rectMaxY),
            glm::vec2(rectMinX, rectMaxY),
        };

        for (std::size_t cornerIndex = 0; cornerIndex < rectCorners.size(); ++cornerIndex)
        {
            const glm::vec2& rectStart = rectCorners[cornerIndex];
            const glm::vec2& rectEnd = rectCorners[(cornerIndex + 1) % rectCorners.size()];
            if (SegmentsIntersect2D(segmentStart, segmentEnd, rectStart, rectEnd))
            {
                return true;
            }
        }

        return false;
    }

    bool TriangleOverlapsScreenRect(
        const glm::vec2& t0,
        const glm::vec2& t1,
        const glm::vec2& t2,
        int projectedVertexCount,
        float rectMinX,
        float rectMinY,
        float rectMaxX,
        float rectMaxY)
    {
        if (projectedVertexCount >= 3)
        {
            const std::array<glm::vec2, 3> triangleVertices = {t0, t1, t2};
            for (const glm::vec2& vertex : triangleVertices)
            {
                if (IsPointInScreenRect(vertex.x, vertex.y, rectMinX, rectMinY, rectMaxX, rectMaxY))
                {
                    return true;
                }
            }

            const std::array<glm::vec2, 4> rectCorners = {
                glm::vec2(rectMinX, rectMinY),
                glm::vec2(rectMaxX, rectMinY),
                glm::vec2(rectMaxX, rectMaxY),
                glm::vec2(rectMinX, rectMaxY),
            };

            for (const glm::vec2& corner : rectCorners)
            {
                if (IsPointInTriangle2D(corner, t0, t1, t2))
                {
                    return true;
                }
            }

            const std::array<std::pair<glm::vec2, glm::vec2>, 3> triangleEdges = {{
                {t0, t1},
                {t1, t2},
                {t2, t0},
            }};

            for (const auto& triangleEdge : triangleEdges)
            {
                for (std::size_t cornerIndex = 0; cornerIndex < rectCorners.size(); ++cornerIndex)
                {
                    const glm::vec2& rectStart = rectCorners[cornerIndex];
                    const glm::vec2& rectEnd = rectCorners[(cornerIndex + 1) % rectCorners.size()];
                    if (SegmentsIntersect2D(
                            triangleEdge.first,
                            triangleEdge.second,
                            rectStart,
                            rectEnd))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        if (projectedVertexCount == 2)
        {
            return SegmentOverlapsScreenRect(t0, t1, rectMinX, rectMinY, rectMaxX, rectMaxY);
        }

        return false;
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

            const glm::vec3 worldVertices[3] = {
                glm::vec3(worldMatrix * glm::vec4(positions[i0], 1.0f)),
                glm::vec3(worldMatrix * glm::vec4(positions[i1], 1.0f)),
                glm::vec3(worldMatrix * glm::vec4(positions[i2], 1.0f)),
            };

            glm::vec2 screenVertices[3];
            bool projectedVertices[3] = {false, false, false};
            for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
            {
                if (!TryWorldToScreenPointRelaxed(
                        worldVertices[vertexIndex],
                        viewMatrix,
                        projectionMatrix,
                        viewportSize,
                        screenVertices[vertexIndex]))
                {
                    continue;
                }

                projectedVertices[vertexIndex] = true;
            }

            const int projectedVertexCount = static_cast<int>(projectedVertices[0])
                + static_cast<int>(projectedVertices[1])
                + static_cast<int>(projectedVertices[2]);

            if (projectedVertexCount == 3
                && TriangleOverlapsScreenRect(
                    screenVertices[0],
                    screenVertices[1],
                    screenVertices[2],
                    projectedVertexCount,
                    rectMinX,
                    rectMinY,
                    rectMaxX,
                    rectMaxY))
            {
                return true;
            }

            if (projectedVertexCount == 1)
            {
                for (int vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
                {
                    if (!projectedVertices[vertexIndex])
                    {
                        continue;
                    }

                    if (IsPointInScreenRect(
                            screenVertices[vertexIndex].x,
                            screenVertices[vertexIndex].y,
                            rectMinX,
                            rectMinY,
                            rectMaxX,
                            rectMaxY))
                    {
                        return true;
                    }
                }
            }

            const std::array<std::pair<int, int>, 3> triangleEdges = {{
                {0, 1},
                {1, 2},
                {2, 0},
            }};

            for (const auto& edge : triangleEdges)
            {
                if (!projectedVertices[edge.first] || !projectedVertices[edge.second])
                {
                    continue;
                }

                if (SegmentOverlapsScreenRect(
                        screenVertices[edge.first],
                        screenVertices[edge.second],
                        rectMinX,
                        rectMinY,
                        rectMaxX,
                        rectMaxY))
                {
                    return true;
                }
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
            if (!TryWorldToScreenPointRelaxed(
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
