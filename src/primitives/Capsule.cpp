#include "primitives/Capsule.h"

#include "engine/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    void AppendHemisphereRing(
        std::vector<float>& vertices,
        float radius,
        float centerY,
        float phi,
        int slices,
        float v)
    {
        for (int slice = 0; slice <= slices; ++slice)
        {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * glm::two_pi<float>();

            const glm::vec3 normal(
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta));

            const glm::vec3 position = glm::vec3(0.0f, centerY, 0.0f) + normal * radius;
            const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

            PrimitiveMesh::PushVertex(vertices, position, normal, glm::vec2(u, v), tangent);
        }
    }
}

std::unique_ptr<Mesh> CreateCapsuleMesh(float radius, float height, int slices, int capStacks)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    const float bodyHalf = std::max(0.0f, height * 0.5f - radius);
    const float topCenterY = bodyHalf;
    const float bottomCenterY = -bodyHalf;

    const int totalRings = capStacks * 2 + 1;
    vertices.reserve(static_cast<std::size_t>((totalRings + 1) * (slices + 1) * 11));
    indices.reserve(static_cast<std::size_t>((totalRings - 1) * slices * 6));

    std::vector<unsigned int> ringStarts;
    ringStarts.reserve(static_cast<std::size_t>(totalRings + 1));

    for (int capStack = capStacks; capStack >= 0; --capStack)
    {
        const float t = static_cast<float>(capStack) / static_cast<float>(capStacks);
        const float phi = t * glm::half_pi<float>();
        const float v = static_cast<float>(capStacks - capStack) / static_cast<float>(capStacks * 2 + 1);

        ringStarts.push_back(static_cast<unsigned int>(vertices.size() / 11));
        AppendHemisphereRing(vertices, radius, topCenterY, phi, slices, v);
    }

    ringStarts.push_back(static_cast<unsigned int>(vertices.size() / 11));
    for (int slice = 0; slice <= slices; ++slice)
    {
        const float u = static_cast<float>(slice) / static_cast<float>(slices);
        const float theta = u * glm::two_pi<float>();
        const glm::vec3 normal(std::cos(theta), 0.0f, std::sin(theta));
        const glm::vec3 position = normal * radius + glm::vec3(0.0f, topCenterY, 0.0f);
        const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

        PrimitiveMesh::PushVertex(
            vertices,
            position,
            normal,
            glm::vec2(u, static_cast<float>(capStacks) / static_cast<float>(capStacks * 2 + 1)),
            tangent);
    }

    ringStarts.push_back(static_cast<unsigned int>(vertices.size() / 11));
    for (int slice = 0; slice <= slices; ++slice)
    {
        const float u = static_cast<float>(slice) / static_cast<float>(slices);
        const float theta = u * glm::two_pi<float>();
        const glm::vec3 normal(std::cos(theta), 0.0f, std::sin(theta));
        const glm::vec3 position = normal * radius + glm::vec3(0.0f, bottomCenterY, 0.0f);
        const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

        PrimitiveMesh::PushVertex(
            vertices,
            position,
            normal,
            glm::vec2(u, static_cast<float>(capStacks + 1) / static_cast<float>(capStacks * 2 + 1)),
            tangent);
    }

    for (int capStack = 1; capStack <= capStacks; ++capStack)
    {
        const float t = static_cast<float>(capStack) / static_cast<float>(capStacks);
        const float phi = glm::pi<float>() - t * glm::half_pi<float>();
        const float v = static_cast<float>(capStacks + 1 + capStack) / static_cast<float>(capStacks * 2 + 1);

        ringStarts.push_back(static_cast<unsigned int>(vertices.size() / 11));
        AppendHemisphereRing(vertices, radius, bottomCenterY, phi, slices, v);
    }

    ringStarts.push_back(static_cast<unsigned int>(vertices.size() / 11));

    for (std::size_t ring = 0; ring + 1 < ringStarts.size(); ++ring)
    {
        const unsigned int ringStart = ringStarts[ring];
        const unsigned int nextRingStart = ringStarts[ring + 1];

        for (int slice = 0; slice < slices; ++slice)
        {
            const unsigned int current = ringStart + static_cast<unsigned int>(slice);
            const unsigned int next = current + 1;
            const unsigned int below = nextRingStart + static_cast<unsigned int>(slice);
            const unsigned int belowNext = below + 1;

            indices.push_back(current);
            indices.push_back(below);
            indices.push_back(next);

            indices.push_back(next);
            indices.push_back(below);
            indices.push_back(belowNext);
        }
    }

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
