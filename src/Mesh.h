#pragma once

class Mesh
{
public:
    Mesh(const float* vertices, unsigned int vertexCount);
    ~Mesh();

    void Draw() const;

private:
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_vertexCount = 0;
};
