#include "engine/rendering/Mesh.h"

#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <d3d12.h>

#include <cmath>
#include <limits>

Mesh::Mesh(
    const float* vertices,
    unsigned int vertexCount,
    unsigned int floatsPerVertex,
    const unsigned int* indices,
    unsigned int indexCount)
    : m_indexCount(indexCount),
      m_floatsPerVertex(floatsPerVertex)
{
    m_vertices.assign(vertices, vertices + static_cast<std::size_t>(vertexCount) * floatsPerVertex);
    m_positions.reserve(vertexCount);
    for (unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const unsigned int offset = vertexIndex * floatsPerVertex;
        m_positions.emplace_back(vertices[offset], vertices[offset + 1], vertices[offset + 2]);
    }

    m_indices.assign(indices, indices + indexCount);

    const std::uint32_t vertexByteSize =
        static_cast<std::uint32_t>(m_vertices.size() * sizeof(float));
    const std::uint32_t indexByteSize =
        static_cast<std::uint32_t>(m_indices.size() * sizeof(unsigned int));
    m_vertexBuffer.Create(GpuBuffer::Type::Vertex, m_vertices.data(), vertexByteSize);
    m_indexBuffer.Create(GpuBuffer::Type::Index, m_indices.data(), indexByteSize);
}

Mesh::~Mesh()
{
    ReleaseGpuResources();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_indexCount(other.m_indexCount),
      m_floatsPerVertex(other.m_floatsPerVertex),
      m_positions(std::move(other.m_positions)),
      m_indices(std::move(other.m_indices)),
      m_vertices(std::move(other.m_vertices)),
      m_vertexBuffer(std::move(other.m_vertexBuffer)),
      m_indexBuffer(std::move(other.m_indexBuffer))
{
    other.m_indexCount = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept
{
    if (this != &other)
    {
        ReleaseGpuResources();
        m_indexCount = other.m_indexCount;
        m_floatsPerVertex = other.m_floatsPerVertex;
        m_positions = std::move(other.m_positions);
        m_indices = std::move(other.m_indices);
        m_vertices = std::move(other.m_vertices);
        m_vertexBuffer = std::move(other.m_vertexBuffer);
        m_indexBuffer = std::move(other.m_indexBuffer);
        other.m_indexCount = 0;
    }

    return *this;
}

std::unique_ptr<Mesh> Mesh::Clone() const
{
    if (m_vertices.empty() || m_indices.empty())
    {
        return nullptr;
    }

    const unsigned int vertexCount =
        static_cast<unsigned int>(m_vertices.size()) / m_floatsPerVertex;
    return std::make_unique<Mesh>(
        m_vertices.data(),
        vertexCount,
        m_floatsPerVertex,
        m_indices.data(),
        m_indexCount);
}

void Mesh::Draw() const
{
    if (!m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid() || m_indexCount == 0)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_vertexBuffer.BindVertex(0, m_floatsPerVertex * static_cast<std::uint32_t>(sizeof(float)));
    m_indexBuffer.BindIndex();
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

bool Mesh::IntersectRay(
    const glm::vec3& localOrigin,
    const glm::vec3& localDirection,
    float& hitDistance) const
{
    hitDistance = std::numeric_limits<float>::max();
    bool hit = false;

    for (std::size_t triangleIndex = 0; triangleIndex + 2 < m_indices.size(); triangleIndex += 3)
    {
        const unsigned int i0 = m_indices[triangleIndex];
        const unsigned int i1 = m_indices[triangleIndex + 1];
        const unsigned int i2 = m_indices[triangleIndex + 2];
        if (i0 >= m_positions.size() || i1 >= m_positions.size() || i2 >= m_positions.size())
        {
            continue;
        }

        const glm::vec3& a = m_positions[i0];
        const glm::vec3& b = m_positions[i1];
        const glm::vec3& c = m_positions[i2];

        const glm::vec3 edge1 = b - a;
        const glm::vec3 edge2 = c - a;
        const glm::vec3 pvec = glm::cross(localDirection, edge2);
        const float det = glm::dot(edge1, pvec);
        if (std::fabs(det) < 1e-8f)
        {
            continue;
        }

        const float invDet = 1.0f / det;
        const glm::vec3 tvec = localOrigin - a;
        const float u = glm::dot(tvec, pvec) * invDet;
        if (u < 0.0f || u > 1.0f)
        {
            continue;
        }

        const glm::vec3 qvec = glm::cross(tvec, edge1);
        const float v = glm::dot(localDirection, qvec) * invDet;
        if (v < 0.0f || u + v > 1.0f)
        {
            continue;
        }

        const float distance = glm::dot(edge2, qvec) * invDet;
        if (distance > 0.0f && distance < hitDistance)
        {
            hitDistance = distance;
            hit = true;
        }
    }

    return hit;
}

const std::vector<glm::vec3>& Mesh::GetPositions() const
{
    return m_positions;
}

const std::vector<unsigned int>& Mesh::GetIndices() const
{
    return m_indices;
}

void Mesh::ReleaseGpuResources()
{
    m_vertexBuffer.Destroy();
    m_indexBuffer.Destroy();
    m_indexCount = 0;
}
