#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

class Camera;
class Shader;

struct SelectionMeshDraw;

class SelectionRenderer
{
public:
    SelectionRenderer();
    ~SelectionRenderer();

    void Draw(const Camera& camera, const std::vector<SelectionMeshDraw>& meshes) const;

private:
    std::unique_ptr<Shader> m_shader;
};
