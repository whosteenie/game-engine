#include "primitives/Sphere.h"

#include "engine/rendering/resources/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    unsigned int AppendRing(
        std::vector<float>& vertices,
        float radius,
        float phi,
        int slices,
        float v)
    {
        const unsigned int ringStart =
            static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);

        for (int slice = 0; slice <= slices; ++slice)
        {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * glm::two_pi<float>();

            const glm::vec3 normal(
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta));

            const glm::vec3 position = normal * radius;
            const glm::vec3 tangent(-std::sin(theta), 0.0f, std::cos(theta));

            PrimitiveMesh::PushVertex(vertices, position, normal, glm::vec2(u, v), glm::vec4(tangent, 1.0f));
        }

        return ringStart;
    }

    void AppendLatBand(
        std::vector<unsigned int>& indices,
        unsigned int upperRingStart,
        unsigned int lowerRingStart,
        int slices)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            const unsigned int upper = upperRingStart + static_cast<unsigned int>(slice);
            const unsigned int upperNext = upper + 1;
            const unsigned int lower = lowerRingStart + static_cast<unsigned int>(slice);
            const unsigned int lowerNext = lower + 1;

            indices.push_back(upper);
            indices.push_back(lower);
            indices.push_back(upperNext);

            indices.push_back(upperNext);
            indices.push_back(lower);
            indices.push_back(lowerNext);
        }
    }

    void AppendPoleFan(
        std::vector<unsigned int>& indices,
        unsigned int poleIndex,
        unsigned int ringStart,
        int slices,
        bool northPole)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            if (northPole)
            {
                indices.push_back(poleIndex);
                indices.push_back(ringStart + static_cast<unsigned int>(slice));
                indices.push_back(ringStart + static_cast<unsigned int>(slice + 1));
            }
            else
            {
                indices.push_back(poleIndex);
                indices.push_back(ringStart + static_cast<unsigned int>(slice + 1));
                indices.push_back(ringStart + static_cast<unsigned int>(slice));
            }
        }
    }
}

std::unique_ptr<Mesh> CreateSphereMesh(float radius, int slices, int stacks)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    if (stacks < 2)
    {
        stacks = 2;
    }

    const unsigned int northPoleIndex =
        static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(0.0f, radius, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.5f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    std::vector<unsigned int> ringStarts;
    ringStarts.reserve(static_cast<std::size_t>(stacks - 1));
    for (int stack = 1; stack < stacks; ++stack)
    {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * glm::pi<float>();
        ringStarts.push_back(AppendRing(vertices, radius, phi, slices, 1.0f - v));
    }

    AppendPoleFan(indices, northPoleIndex, ringStarts.front(), slices, true);
    for (int stack = 0; stack + 1 < stacks - 1; ++stack)
    {
        AppendLatBand(indices, ringStarts[static_cast<std::size_t>(stack)], ringStarts[static_cast<std::size_t>(stack + 1)], slices);
    }

    const unsigned int southPoleIndex =
        static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(0.0f, -radius, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec2(0.5f, 1.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    AppendPoleFan(indices, southPoleIndex, ringStarts.back(), slices, false);

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
