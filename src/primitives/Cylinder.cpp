#include "primitives/Cylinder.h"

#include "engine/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <memory>
#include <vector>

std::unique_ptr<Mesh> CreateCylinderMesh(float radius, float height, int slices)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    const float halfHeight = height * 0.5f;

    for (int ring = 0; ring <= 1; ++ring)
    {
        const float y = ring == 0 ? -halfHeight : halfHeight;
        const float v = static_cast<float>(ring);

        for (int slice = 0; slice <= slices; ++slice)
        {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * glm::two_pi<float>();

            const glm::vec3 normal(std::cos(theta), 0.0f, std::sin(theta));
            const glm::vec3 position = normal * radius + glm::vec3(0.0f, y, 0.0f);
            const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

            PrimitiveMesh::PushVertex(vertices, position, normal, glm::vec2(u, v), tangent);
        }
    }

    for (int slice = 0; slice < slices; ++slice)
    {
        const unsigned int bottom = static_cast<unsigned int>(slice);
        const unsigned int top = bottom + static_cast<unsigned int>(slices + 1);

        indices.push_back(bottom);
        indices.push_back(top);
        indices.push_back(bottom + 1);

        indices.push_back(bottom + 1);
        indices.push_back(top);
        indices.push_back(top + 1);
    }

    const unsigned int topCenterIndex = static_cast<unsigned int>(vertices.size() / 11);
    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(0.0f, halfHeight, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.5f, 0.5f),
        glm::vec3(1.0f, 0.0f, 0.0f));

    const unsigned int topCapRingStart = static_cast<unsigned int>(vertices.size() / 11);
    for (int slice = 0; slice <= slices; ++slice)
    {
        const float u = static_cast<float>(slice) / static_cast<float>(slices);
        const float theta = u * glm::two_pi<float>();
        const glm::vec3 position(radius * std::cos(theta), halfHeight, radius * std::sin(theta));
        const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

        PrimitiveMesh::PushVertex(vertices, position, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(u, 1.0f), tangent);
    }

    for (int slice = 0; slice < slices; ++slice)
    {
        indices.push_back(topCenterIndex);
        indices.push_back(topCapRingStart + static_cast<unsigned int>(slice));
        indices.push_back(topCapRingStart + static_cast<unsigned int>(slice + 1));
    }

    const unsigned int bottomCenterIndex = static_cast<unsigned int>(vertices.size() / 11);
    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(0.0f, -halfHeight, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec2(0.5f, 0.5f),
        glm::vec3(1.0f, 0.0f, 0.0f));

    const unsigned int bottomCapRingStart = static_cast<unsigned int>(vertices.size() / 11);
    for (int slice = 0; slice <= slices; ++slice)
    {
        const float u = static_cast<float>(slice) / static_cast<float>(slices);
        const float theta = u * glm::two_pi<float>();
        const glm::vec3 position(radius * std::cos(theta), -halfHeight, radius * std::sin(theta));
        const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

        PrimitiveMesh::PushVertex(vertices, position, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(u, 0.0f), tangent);
    }

    for (int slice = 0; slice < slices; ++slice)
    {
        indices.push_back(bottomCenterIndex);
        indices.push_back(bottomCapRingStart + static_cast<unsigned int>(slice + 1));
        indices.push_back(bottomCapRingStart + static_cast<unsigned int>(slice));
    }

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
