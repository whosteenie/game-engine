#include "primitives/Plane.h"

#include "engine/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <memory>
#include <vector>

std::unique_ptr<Mesh> CreatePlaneMesh(float halfExtent)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    const float tileScale = 3.0f;
    const float maxUv = (halfExtent * 2.0f) / tileScale;

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(-halfExtent, 0.0f, -halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.0f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(halfExtent, 0.0f, -halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(maxUv, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(halfExtent, 0.0f, halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(maxUv, maxUv),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(-halfExtent, 0.0f, halfExtent),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.0f, maxUv),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    indices = {0, 1, 2, 2, 3, 0};

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
