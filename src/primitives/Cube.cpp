#include "primitives/Cube.h"

#include "engine/rendering/resources/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/glm.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
    glm::vec4 ComputeFaceTangent(
        const glm::vec3& normal,
        const glm::vec3& c0,
        const glm::vec3& c1,
        const glm::vec3& c3)
    {
        glm::vec3 tangent = c1 - c0;
        tangent -= normal * glm::dot(normal, tangent);
        const float tangentLengthSquared = glm::dot(tangent, tangent);
        if (tangentLengthSquared > 1.0e-12f)
        {
            tangent /= std::sqrt(tangentLengthSquared);
        }
        else
        {
            const glm::vec3 fallbackAxis =
                std::abs(normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            tangent = glm::normalize(glm::cross(fallbackAxis, normal));
        }

        glm::vec3 bitangentReference = c3 - c0;
        bitangentReference -= normal * glm::dot(normal, bitangentReference);
        const float bitangentLengthSquared = glm::dot(bitangentReference, bitangentReference);
        if (bitangentLengthSquared > 1.0e-12f)
        {
            bitangentReference /= std::sqrt(bitangentLengthSquared);
        }

        const float handedness =
            glm::dot(glm::cross(normal, tangent), bitangentReference) >= 0.0f ? 1.0f : -1.0f;
        return glm::vec4(tangent, handedness);
    }

    void AppendQuad(
        std::vector<float>& vertices,
        std::vector<unsigned int>& indices,
        const glm::vec3& normal,
        const glm::vec3& c0,
        const glm::vec3& c1,
        const glm::vec3& c2,
        const glm::vec3& c3,
        const glm::vec2& uv0,
        const glm::vec2& uv1,
        const glm::vec2& uv2,
        const glm::vec2& uv3)
    {
        const unsigned int base =
            static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
        const glm::vec4 tangent = ComputeFaceTangent(normal, c0, c1, c3);

        PrimitiveMesh::PushVertex(vertices, c0, normal, uv0, tangent);
        PrimitiveMesh::PushVertex(vertices, c1, normal, uv1, tangent);
        PrimitiveMesh::PushVertex(vertices, c2, normal, uv2, tangent);
        PrimitiveMesh::PushVertex(vertices, c3, normal, uv3, tangent);

        const glm::vec3 faceNormal = glm::cross(c1 - c0, c2 - c0);
        const bool windingMatchesNormal = glm::dot(faceNormal, normal) > 0.0f;
        if (!windingMatchesNormal)
        {
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        }
        else
        {
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 1);
            indices.push_back(base + 0);
            indices.push_back(base + 3);
            indices.push_back(base + 2);
        }
    }
}

std::unique_ptr<Mesh> CreateCubeMesh()
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    const glm::vec2 uv00(0.0f, 0.0f);
    const glm::vec2 uv10(1.0f, 0.0f);
    const glm::vec2 uv11(1.0f, 1.0f);
    const glm::vec2 uv01(0.0f, 1.0f);
    const glm::vec2 uvBottomLeft(0.0f, 1.0f);
    const glm::vec2 uvBottomRight(1.0f, 1.0f);
    const glm::vec2 uvTopRight(1.0f, 0.0f);
    const glm::vec2 uvTopLeft(0.0f, 0.0f);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        uv00,
        uv10,
        uv11,
        uv01);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        uv00,
        uv10,
        uv11,
        uv01);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        uv10,
        uv11,
        uv01,
        uv00);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        uv10,
        uv11,
        uv01,
        uv00);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(-0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, -0.5f),
        glm::vec3(0.5f, -0.5f, 0.5f),
        glm::vec3(-0.5f, -0.5f, 0.5f),
        uvBottomLeft,
        uvBottomRight,
        uvTopRight,
        uvTopLeft);

    AppendQuad(
        vertices,
        indices,
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(-0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, -0.5f),
        glm::vec3(0.5f, 0.5f, 0.5f),
        glm::vec3(-0.5f, 0.5f, 0.5f),
        uvBottomLeft,
        uvBottomRight,
        uvTopRight,
        uvTopLeft);

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
