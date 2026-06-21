#include "primitives/Floor.h"

#include "engine/Mesh.h"

#include <memory>

std::unique_ptr<Mesh> CreateFloorMesh(float halfExtent)
{
    float vertices[] = {
        -halfExtent, 0.0f, -halfExtent,  0.0f, 1.0f, 0.0f,
         halfExtent, 0.0f, -halfExtent,  0.0f, 1.0f, 0.0f,
         halfExtent, 0.0f,  halfExtent,  0.0f, 1.0f, 0.0f,
        -halfExtent, 0.0f,  halfExtent,  0.0f, 1.0f, 0.0f,
    };

    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    return std::make_unique<Mesh>(vertices, 4, indices, 6);
}
