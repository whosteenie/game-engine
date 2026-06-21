#include "engine/ScenePicker.h"

#include "engine/SceneObject.h"

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

int PickSceneObject(
    const std::vector<SceneObject>& objects,
    const Ray& ray)
{
    int closestIndex = -1;
    float closestDistance = std::numeric_limits<float>::max();

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        object.GetWorldBounds(boundsMin, boundsMax);

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
