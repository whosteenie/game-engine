#pragma once

#include <glm/glm.hpp>
#include <vector>

struct Ray
{
    glm::vec3 origin;
    glm::vec3 direction;
};

class SceneObject;

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

float DistanceRayLineSegment(
    const Ray& ray,
    const glm::vec3& segmentStart,
    const glm::vec3& segmentEnd);

bool IntersectRayPlane(
    const Ray& ray,
    const glm::vec3& planePoint,
    const glm::vec3& planeNormal,
    float& hitDistance);

glm::vec3 ClosestPointOnRay(const Ray& ray, float distance);
glm::vec3 ClosestPointOnLine(const glm::vec3& linePoint, const glm::vec3& lineDirection, const glm::vec3& point);

int PickSceneObject(
    const std::vector<SceneObject>& objects,
    const Ray& ray);
