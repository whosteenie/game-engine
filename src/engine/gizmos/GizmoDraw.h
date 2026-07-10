#pragma once

#include <glm/glm.hpp>
#include <vector>

class Camera;
class Shader;

namespace GizmoDraw
{
    void DrawLineVertices(
        const Shader& shader,
        const Camera& camera,
        const std::vector<float>& vertices,
        const glm::vec3& color,
        bool depthReadOnly = true);
}
