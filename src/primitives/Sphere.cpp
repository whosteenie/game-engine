#include "primitives/Sphere.h"

#include "engine/Mesh.h"
#include "primitives/PrimitiveMeshUtils.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <memory>
#include <vector>

std::unique_ptr<Mesh> CreateSphereMesh(float radius, int slices, int stacks)
{
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    vertices.reserve(static_cast<std::size_t>((stacks + 1) * (slices + 1) * 11));
    indices.reserve(static_cast<std::size_t>(stacks * slices * 6));

    for (int stack = 0; stack <= stacks; ++stack)
    {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * glm::pi<float>();

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

            PrimitiveMesh::PushVertex(vertices, position, normal, glm::vec2(u, 1.0f - v), tangent);
        }
    }

    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            const unsigned int first = static_cast<unsigned int>(stack * (slices + 1) + slice);
            const unsigned int second = first + static_cast<unsigned int>(slices + 1);

            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(first + 1);

            indices.push_back(first + 1);
            indices.push_back(second);
            indices.push_back(second + 1);
        }
    }

    return PrimitiveMesh::BuildMesh(vertices, indices);
}
