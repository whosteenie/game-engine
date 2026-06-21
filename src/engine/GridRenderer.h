#pragma once

class Camera;
class Shader;

class GridRenderer
{
public:
    GridRenderer();
    ~GridRenderer();

    void Draw(const Camera& camera) const;

private:
    void BuildGridGeometry(int halfExtent, float spacing);

    Shader* m_shader = nullptr;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_vertexCount = 0;
};