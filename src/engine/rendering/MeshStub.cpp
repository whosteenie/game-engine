#include "engine/rendering/Mesh.h"

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
}

Mesh::~Mesh()
{
    DestroyGlResources();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_vao(other.m_vao),
      m_vbo(other.m_vbo),
      m_ebo(other.m_ebo),
      m_indexCount(other.m_indexCount),
      m_floatsPerVertex(other.m_floatsPerVertex),
      m_positions(std::move(other.m_positions)),
      m_indices(std::move(other.m_indices)),
      m_vertices(std::move(other.m_vertices))
{
    other.m_vao = 0;
    other.m_vbo = 0;
    other.m_ebo = 0;
    other.m_indexCount = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept
{
    if (this != &other)
    {
        DestroyGlResources();
        m_vao = other.m_vao;
        m_vbo = other.m_vbo;
        m_ebo = other.m_ebo;
        m_indexCount = other.m_indexCount;
        m_floatsPerVertex = other.m_floatsPerVertex;
        m_positions = std::move(other.m_positions);
        m_indices = std::move(other.m_indices);
        m_vertices = std::move(other.m_vertices);
        other.m_vao = 0;
        other.m_vbo = 0;
        other.m_ebo = 0;
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
        const glm::vec3& a = m_positions[m_indices[triangleIndex]];
        const glm::vec3& b = m_positions[m_indices[triangleIndex + 1]];
        const glm::vec3& c = m_positions[m_indices[triangleIndex + 2]];

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

void Mesh::DestroyGlResources()
{
    m_vao = 0;
    m_vbo = 0;
    m_ebo = 0;
}
