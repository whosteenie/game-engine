#include "primitives/Cube.h"

#include "engine/assets/TangentSpace.h"
#include "engine/rendering/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace
{
    void PushFace(
        std::vector<float>& vertices,
        const glm::vec3& normal,
        const glm::vec3& v0,
        const glm::vec3& v1,
        const glm::vec3& v2,
        const glm::vec3& v3,
        const glm::vec2& uv0,
        const glm::vec2& uv1,
        const glm::vec2& uv2,
        const glm::vec2& uv3)
    {
        const glm::vec4 placeholderTangent(1.0f, 0.0f, 0.0f, 1.0f);
        PrimitiveMesh::PushVertex(vertices, v0, normal, uv0, placeholderTangent);
        PrimitiveMesh::PushVertex(vertices, v1, normal, uv1, placeholderTangent);
        PrimitiveMesh::PushVertex(vertices, v2, normal, uv2, placeholderTangent);
        PrimitiveMesh::PushVertex(vertices, v3, normal, uv3, placeholderTangent);
    }
}

std::unique_ptr<Mesh> CreateCubeMesh()
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    // Outward CCW winding (FrontCounterClockwise = TRUE) for every face.
    PushFace(
        vertices,
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        glm::vec2(0.0f, 0.0f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(0.0f, 1.0f));

    PushFace(
        vertices,
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        glm::vec2(0.0f, 0.0f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(0.0f, 1.0f));

    PushFace(
        vertices,
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(0.0f, 1.0f),
        glm::vec2(0.0f, 0.0f));

    PushFace(
        vertices,
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(0.0f, 1.0f),
        glm::vec2(0.0f, 0.0f));

    PushFace(
        vertices,
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        glm::vec2(0.0f, 1.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(0.0f, 0.0f));

    PushFace(
        vertices,
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        glm::vec2(0.0f, 1.0f),
        glm::vec2(1.0f, 1.0f),
        glm::vec2(1.0f, 0.0f),
        glm::vec2(0.0f, 0.0f));

    indices = {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 6, 7, 4,
        8, 9, 10, 10, 11, 8,
        12, 14, 13, 12, 15, 14,
        16, 17, 18, 18, 19, 16,
        20, 22, 21, 20, 23, 22,
    };

    TangentSpace::GenerateMikkTSpaceTangents(vertices, indices);

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
