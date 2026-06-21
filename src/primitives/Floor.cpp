#include "primitives/Floor.h"

#include "engine/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <memory>
#include <vector>

std::unique_ptr<Mesh> CreateFloorMesh(float halfExtent)
{
    const float tileScale = 3.0f;
    const float maxUv = (halfExtent * 2.0f) / tileScale;
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);

    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(-halfExtent, 0.0f, -halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.0f, 0.0f),
        tangent);

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(halfExtent, 0.0f, -halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(maxUv, 0.0f),
        tangent);

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(halfExtent, 0.0f, halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(maxUv, maxUv),
        tangent);

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(-halfExtent, 0.0f, halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.0f, maxUv),
        tangent);

    indices = {0, 1, 2, 2, 3, 0};

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
