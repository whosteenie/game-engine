#include "primitives/PrimitiveMeshUtils.h"

#include "engine/Mesh.h"

#include <memory>

namespace PrimitiveMesh
{
    void PushVertex(
        std::vector<float>& vertices,
        const glm::vec3& position,
        const glm::vec3& normal,
        const glm::vec2& uv0,
        const glm::vec4& tangent,
        const glm::vec2& uv1,
        bool hasSecondUv)
    {
        const glm::vec2 resolvedUv1 = hasSecondUv ? uv1 : uv0;

        vertices.push_back(position.x);
        vertices.push_back(position.y);
        vertices.push_back(position.z);
        vertices.push_back(normal.x);
        vertices.push_back(normal.y);
        vertices.push_back(normal.z);
        vertices.push_back(uv0.x);
        vertices.push_back(uv0.y);
        vertices.push_back(resolvedUv1.x);
        vertices.push_back(resolvedUv1.y);
        vertices.push_back(tangent.x);
        vertices.push_back(tangent.y);
        vertices.push_back(tangent.z);
        vertices.push_back(tangent.w);
    }

    std::unique_ptr<Mesh> BuildMesh(
        const std::vector<float>& vertices,
        const std::vector<unsigned int>& indices)
    {
        const unsigned int vertexCount = static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
        return std::make_unique<Mesh>(
            vertices.data(),
            vertexCount,
            Mesh::TexturedVertexFloatCount,
            indices.data(),
            static_cast<unsigned int>(indices.size()));
    }
}
