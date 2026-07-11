#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/rhi/d3d12/GpuBuffer.h"

struct Meshlet
{
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t triangleCount = 0;
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 0.0f;
    glm::vec3 boundsMin{0.0f};
    std::uint32_t flags = 0;
    glm::vec3 boundsMax{0.0f};
    std::uint32_t pad0 = 0;
};

struct MeshletTriangle
{
    std::uint32_t v0 = 0;
    std::uint32_t v1 = 0;
    std::uint32_t v2 = 0;
    std::uint32_t pad0 = 0;
};

class Mesh
{
public:
    static constexpr unsigned int BasicVertexFloatCount = 6;
    static constexpr unsigned int TexturedVertexFloatCount = 14;
    static constexpr std::uint32_t MaxMeshletVertices = 64;
    static constexpr std::uint32_t MaxMeshletTriangles = 64;

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
    const std::vector<float>& GetVertexData() const { return m_vertices; }
    const std::vector<Meshlet>& GetMeshlets() const { return m_meshlets; }
    const std::vector<std::uint32_t>& GetMeshletVertices() const { return m_meshletVertices; }
    const std::vector<MeshletTriangle>& GetMeshletTriangles() const { return m_meshletTriangles; }
    std::uint32_t GetMeshletCount() const { return static_cast<std::uint32_t>(m_meshlets.size()); }
    std::uint32_t GetMeshletTriangleCount() const { return static_cast<std::uint32_t>(m_meshletTriangles.size()); }
    std::uint32_t GetMeshletVertexReferenceCount() const
    {
        return static_cast<std::uint32_t>(m_meshletVertices.size());
    }

    void EnsureGpuResources() const;
    const GpuBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
    const GpuBuffer& GetIndexBuffer() const { return m_indexBuffer; }
    const GpuBuffer& GetMeshletBuffer() const { return m_meshletBuffer; }
    const GpuBuffer& GetMeshletVertexBuffer() const { return m_meshletVertexBuffer; }
    const GpuBuffer& GetMeshletTriangleBuffer() const { return m_meshletTriangleBuffer; }
    unsigned int GetFloatsPerVertex() const { return m_floatsPerVertex; }

private:
    void BakeMeshlets();
    void ReleaseGpuResources();

    unsigned int m_indexCount = 0;
    unsigned int m_floatsPerVertex = BasicVertexFloatCount;
    std::vector<glm::vec3> m_positions;
    std::vector<unsigned int> m_indices;
    std::vector<float> m_vertices;
    std::vector<Meshlet> m_meshlets;
    std::vector<std::uint32_t> m_meshletVertices;
    std::vector<MeshletTriangle> m_meshletTriangles;
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
    GpuBuffer m_meshletBuffer;
    GpuBuffer m_meshletVertexBuffer;
    GpuBuffer m_meshletTriangleBuffer;
};
