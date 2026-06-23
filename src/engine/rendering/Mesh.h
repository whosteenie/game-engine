#pragma once

#include <glm/glm.hpp>
#include <vector>

class Mesh
{
public:
    static constexpr unsigned int BasicVertexFloatCount = 6;
    static constexpr unsigned int TexturedVertexFloatCount = 14;

    Mesh(
        const float* vertices,
        unsigned int vertexCount,
        unsigned int floatsPerVertex,
        const unsigned int* indices,
        unsigned int indexCount);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void Draw() const;

    bool IntersectRay(
        const glm::vec3& localOrigin,
        const glm::vec3& localDirection,
        float& hitDistance) const;

    const std::vector<glm::vec3>& GetPositions() const;
    const std::vector<unsigned int>& GetIndices() const;

private:
    void DestroyGlResources();

    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_indexCount = 0;
    std::vector<glm::vec3> m_positions;
    std::vector<unsigned int> m_indices;
};
