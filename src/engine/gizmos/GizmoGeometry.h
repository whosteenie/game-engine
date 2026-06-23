#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace GizmoGeometry
{
    void AppendLine(std::vector<float>& lineVertices, const glm::vec3& a, const glm::vec3& b);

    void AppendOrientedBoxOutline(
        std::vector<float>& lineVertices,
        const glm::mat4& modelMatrix,
        const glm::vec3& localBoundsMin,
        const glm::vec3& localBoundsMax,
        float padding);
}
