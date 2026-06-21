#include "primitives/Triangle.h"

#include "Mesh.h"

#include <memory>

std::unique_ptr<Mesh> CreateTriangleMesh()
{
    const float k = 0.4330127f;
    float vertices[] = {
         0.0f,  0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
        -k,    -0.25f, 0.0f,  0.0f, 0.0f, 1.0f,
         k,    -0.25f, 0.0f,  0.0f, 0.0f, 1.0f,
    };

    unsigned int indices[] = { 0, 1, 2 };

    return std::make_unique<Mesh>(vertices, 3, indices, 3);
}
