#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "engine/rhi/d3d12/GpuBuffer.h"

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

    std::unique_ptr<Mesh> Clone() const;

    void Draw() const;

    bool IntersectRay(
        const glm::vec3& localOrigin,
        const glm::vec3& localDirection,
        float& hitDistance) const;

    const std::vector<glm::vec3>& GetPositions() const;
    const std::vector<unsigned int>& GetIndices() const;

    void EnsureGpuResources() const;
    const GpuBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
    const GpuBuffer& GetIndexBuffer() const { return m_indexBuffer; }
    unsigned int GetFloatsPerVertex() const { return m_floatsPerVertex; }

private:
    void ReleaseGpuResources();

    unsigned int m_indexCount = 0;
    unsigned int m_floatsPerVertex = BasicVertexFloatCount;
    std::vector<glm::vec3> m_positions;
    std::vector<unsigned int> m_indices;
    std::vector<float> m_vertices;
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
};
