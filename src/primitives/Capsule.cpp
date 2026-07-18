#include "primitives/Capsule.h"

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
        float centerY,
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

            const glm::vec3 position = glm::vec3(0.0f, centerY, 0.0f) + normal * radius;
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

            // CCW when viewed from outside (FrontCounterClockwise = TRUE).
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
                // Match CreateCylinderMesh top cap: CCW when viewed from outside (+Y).
                indices.push_back(poleIndex);
                indices.push_back(ringStart + static_cast<unsigned int>(slice));
                indices.push_back(ringStart + static_cast<unsigned int>(slice + 1));
            }
            else
            {
                // Match CreateCylinderMesh bottom cap: CCW when viewed from outside (-Y).
                indices.push_back(poleIndex);
                indices.push_back(ringStart + static_cast<unsigned int>(slice + 1));
                indices.push_back(ringStart + static_cast<unsigned int>(slice));
            }
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
    const float vScale = static_cast<float>(capStacks * 2 + 1);

    const unsigned int topPoleIndex =
        static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
    PrimitiveMesh::PushVertex(
        vertices,
        glm::vec3(0.0f, topCenterY + radius, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec2(0.5f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    std::vector<unsigned int> topRingStarts;
    topRingStarts.reserve(static_cast<std::size_t>(capStacks));
    for (int capStack = 0; capStack < capStacks; ++capStack)
    {
        const float phi = static_cast<float>(capStack + 1) / static_cast<float>(capStacks) * glm::half_pi<float>();
        const float v = static_cast<float>(capStack + 1) / vScale;
        topRingStarts.push_back(AppendRing(vertices, radius, topCenterY, phi, slices, v));
    }

    AppendPoleFan(indices, topPoleIndex, topRingStarts.front(), slices, true);
    for (int capStack = 0; capStack + 1 < capStacks; ++capStack)
    {
        AppendLatBand(indices, topRingStarts[static_cast<std::size_t>(capStack)], topRingStarts[static_cast<std::size_t>(capStack + 1)], slices);
    }

    const unsigned int topEquatorRing = topRingStarts.back();

    unsigned int bottomEquatorRing = topEquatorRing;
    if (bodyHalf > 0.0f)
    {
        bottomEquatorRing = AppendRing(
            vertices,
            radius,
            bottomCenterY,
            glm::half_pi<float>(),
            slices,
            static_cast<float>(capStacks + 1) / vScale);
        AppendLatBand(indices, topEquatorRing, bottomEquatorRing, slices);
    }

    std::vector<unsigned int> bottomRingStarts;
    bottomRingStarts.reserve(static_cast<std::size_t>(capStacks));
    for (int capStack = 0; capStack < capStacks; ++capStack)
    {
        const float phi =
            glm::half_pi<float>()
            + static_cast<float>(capStack + 1) / static_cast<float>(capStacks) * glm::half_pi<float>();
        const float v = static_cast<float>(capStacks + 1 + capStack + 1) / vScale;
        bottomRingStarts.push_back(AppendRing(vertices, radius, bottomCenterY, phi, slices, v));
    }

    if (!bottomRingStarts.empty())
    {
        AppendLatBand(indices, bottomEquatorRing, bottomRingStarts.front(), slices);
        for (int capStack = 0; capStack + 1 < capStacks; ++capStack)
        {
            AppendLatBand(
                indices,
                bottomRingStarts[static_cast<std::size_t>(capStack)],
                bottomRingStarts[static_cast<std::size_t>(capStack + 1)],
                slices);
        }

        const unsigned int bottomPoleIndex =
            static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
        PrimitiveMesh::PushVertex(
            vertices,
            glm::vec3(0.0f, bottomCenterY - radius, 0.0f),
            glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec2(0.5f, 1.0f),
            glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

        AppendPoleFan(indices, bottomPoleIndex, bottomRingStarts.back(), slices, false);
    }

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
