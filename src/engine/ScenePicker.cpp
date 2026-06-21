#include "engine/ScenePicker.h"

#include "engine/SceneObject.h"

#include <algorithm>
#include <cmath>
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

float DistanceRayLineSegment(
    const Ray& ray,
    const glm::vec3& segmentStart,
    const glm::vec3& segmentEnd)
{
    const glm::vec3 segment = segmentEnd - segmentStart;
    const glm::vec3 w0 = ray.origin - segmentStart;

    const float segmentLengthSquared = glm::dot(segment, segment);
    if (segmentLengthSquared < 0.000001f)
    {
        return glm::length(glm::cross(ray.direction, w0));
    }

    const float rayProjection = glm::dot(ray.direction, segment);
    const float denominator = 1.0f - rayProjection * rayProjection;

    float rayParameter = 0.0f;
    float segmentParameter = 0.0f;

    if (denominator > 0.000001f)
    {
        rayParameter = (rayProjection * glm::dot(segment, w0) - glm::dot(ray.direction, w0)) / denominator;
        segmentParameter = (glm::dot(segment, w0) - rayProjection * glm::dot(ray.direction, w0)) / denominator;
    }

    rayParameter = std::max(rayParameter, 0.0f);
    segmentParameter = glm::clamp(segmentParameter, 0.0f, 1.0f);

    const glm::vec3 closestRayPoint = ray.origin + ray.direction * rayParameter;
    const glm::vec3 closestSegmentPoint = segmentStart + segment * segmentParameter;
    return glm::length(closestRayPoint - closestSegmentPoint);
}

bool IntersectRayPlane(
    const Ray& ray,
    const glm::vec3& planePoint,
    const glm::vec3& planeNormal,
    float& hitDistance)
{
    const float denominator = glm::dot(planeNormal, ray.direction);
    if (std::abs(denominator) < 0.000001f)
    {
        return false;
    }

    hitDistance = glm::dot(planePoint - ray.origin, planeNormal) / denominator;
    return hitDistance >= 0.0f;
}

glm::vec3 ClosestPointOnRay(const Ray& ray, float distance)
{
    return ray.origin + ray.direction * distance;
}

glm::vec3 ClosestPointOnLine(const glm::vec3& linePoint, const glm::vec3& lineDirection, const glm::vec3& point)
{
    const float directionLengthSquared = glm::dot(lineDirection, lineDirection);
    if (directionLengthSquared < 0.000001f)
    {
        return linePoint;
    }

    const float t = glm::dot(point - linePoint, lineDirection) / directionLengthSquared;
    return linePoint + lineDirection * t;
}

int PickSceneObject(
    const std::vector<SceneObject>& objects,
    const Ray& ray,
    double animationTime)
{
    int closestIndex = -1;
    float closestDistance = std::numeric_limits<float>::max();

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        object.GetWorldBounds(animationTime, boundsMin, boundsMax);

        float hitDistance = 0.0f;
        if (!IntersectRayAabb(ray, boundsMin, boundsMax, hitDistance))
        {
            continue;
        }

        if (hitDistance < closestDistance)
        {
            closestDistance = hitDistance;
            closestIndex = objectIndex;
        }
    }

    return closestIndex;
}
