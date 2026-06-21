#pragma once

class Mesh
{
public:
    static constexpr unsigned int BasicVertexFloatCount = 6;
    static constexpr unsigned int TexturedVertexFloatCount = 11;

    Mesh(
        const float* vertices,
        unsigned int vertexCount,
        unsigned int floatsPerVertex,
        const unsigned int* indices,
        unsigned int indexCount);
    ~Mesh();

    void Draw() const;

private:
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_indexCount = 0;
};
