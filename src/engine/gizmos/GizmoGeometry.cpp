#include "engine/gizmos/GizmoGeometry.h"

void GizmoGeometry::AppendLine(std::vector<float>& vertices, const glm::vec3& a, const glm::vec3& b)
{
    vertices.push_back(a.x);
    vertices.push_back(a.y);
    vertices.push_back(a.z);
    vertices.push_back(b.x);
    vertices.push_back(b.y);
    vertices.push_back(b.z);
}

void GizmoGeometry::AppendOrientedBoxOutline(
    std::vector<float>& lineVertices,
    const glm::mat4& modelMatrix,
    const glm::vec3& localBoundsMin,
    const glm::vec3& localBoundsMax,
    float padding)
{
    const glm::vec3 paddedMin = localBoundsMin - glm::vec3(padding);
    const glm::vec3 paddedMax = localBoundsMax + glm::vec3(padding);

    const glm::vec3 localCorners[8] = {
        { paddedMin.x, paddedMin.y, paddedMin.z },
        { paddedMax.x, paddedMin.y, paddedMin.z },
        { paddedMax.x, paddedMax.y, paddedMin.z },
        { paddedMin.x, paddedMax.y, paddedMin.z },
        { paddedMin.x, paddedMin.y, paddedMax.z },
        { paddedMax.x, paddedMin.y, paddedMax.z },
        { paddedMax.x, paddedMax.y, paddedMax.z },
        { paddedMin.x, paddedMax.y, paddedMax.z },
    };

    glm::vec3 worldCorners[8];
    for (int corner = 0; corner < 8; ++corner)
    {
        worldCorners[corner] = glm::vec3(modelMatrix * glm::vec4(localCorners[corner], 1.0f));
    }

    const int edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };

    for (const auto& edge : edges)
    {
        GizmoGeometry::AppendLine(lineVertices, worldCorners[edge[0]], worldCorners[edge[1]]);
    }
}
