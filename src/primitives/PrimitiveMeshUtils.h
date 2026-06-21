#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

class Mesh;

namespace PrimitiveMesh
{
    void PushVertex(
        std::vector<float>& vertices,
        const glm::vec3& position,
        const glm::vec3& normal,
        const glm::vec2& uv0,
        const glm::vec4& tangent,
        const glm::vec2& uv1 = glm::vec2(0.0f),
        bool hasSecondUv = false);

    std::unique_ptr<Mesh> BuildMesh(
        const std::vector<float>& vertices,
        const std::vector<unsigned int>& indices);
}
