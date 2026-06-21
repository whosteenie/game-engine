#pragma once
class Mesh
{
public:
    Mesh(const float* vertices, unsigned int vertexCount,
         const unsigned int* indices, unsigned int indexCount);
    ~Mesh();
    void Draw() const;
private:
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_indexCount = 0;
};