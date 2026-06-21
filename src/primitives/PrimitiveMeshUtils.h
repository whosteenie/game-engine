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
        const glm::vec2& uv,
        const glm::vec3& tangent);

    std::unique_ptr<Mesh> BuildMesh(
        const std::vector<float>& vertices,
        const std::vector<unsigned int>& indices);
}
