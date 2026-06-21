#pragma once

#include <memory>

class Camera;
class Shader;

class GridRenderer
{
public:
    GridRenderer();
    ~GridRenderer();

    void Draw(const Camera& camera) const;

private:
    void BuildGridGeometry(float halfExtent);

    std::unique_ptr<Shader> m_shader;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_indexCount = 0;

    float m_halfExtent = 14.0f;
    float m_cellSize = 1.0f;
    float m_majorInterval = 10.0f;
};
