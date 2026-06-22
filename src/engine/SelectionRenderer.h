#pragma once

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
    void DrawOutline(
        const Camera& camera,
        const std::vector<SelectionMeshDraw>& meshes,
        int width,
        int height) const;

    std::unique_ptr<Shader> m_outlineShader;
};
