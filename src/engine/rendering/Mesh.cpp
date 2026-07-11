#include "engine/rendering/Mesh.h"

#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace
{
    struct MeshletBuildState
    {
        Meshlet meshlet{};
        std::vector<std::uint32_t> localToGlobal;
        std::unordered_map<std::uint32_t, std::uint32_t> globalToLocal;
        std::vector<MeshletTriangle> triangles;
    };

    bool IsDegenerateTriangle(const std::uint32_t i0, const std::uint32_t i1, const std::uint32_t i2)
    {
        return i0 == i1 || i1 == i2 || i0 == i2;
    }

    bool CanAddTriangle(
        const MeshletBuildState& state,
        const std::uint32_t i0,
        const std::uint32_t i1,
        const std::uint32_t i2)
    {
        if (state.triangles.size() >= Mesh::MaxMeshletTriangles)
        {
            return false;
        }

        std::uint32_t addedVertexCount = 0;
        const std::uint32_t indices[3] = {i0, i1, i2};
        for (const std::uint32_t index : indices)
        {
            if (state.globalToLocal.find(index) == state.globalToLocal.end())
            {
                ++addedVertexCount;
            }
        }

        return state.localToGlobal.size() + addedVertexCount <= Mesh::MaxMeshletVertices;
    }

    std::uint32_t AddVertexReference(MeshletBuildState& state, const std::uint32_t globalIndex)
    {
        const auto existing = state.globalToLocal.find(globalIndex);
        if (existing != state.globalToLocal.end())
        {
            return existing->second;
        }

        const std::uint32_t localIndex = static_cast<std::uint32_t>(state.localToGlobal.size());
        state.localToGlobal.push_back(globalIndex);
        state.globalToLocal.emplace(globalIndex, localIndex);
        return localIndex;
    }

    void AddTriangle(
        MeshletBuildState& state,
        const std::uint32_t i0,
        const std::uint32_t i1,
        const std::uint32_t i2)
    {
        const std::uint32_t local0 = AddVertexReference(state, i0);
        const std::uint32_t local1 = AddVertexReference(state, i1);
        const std::uint32_t local2 = AddVertexReference(state, i2);
        state.triangles.push_back(MeshletTriangle{local0, local1, local2, 0});
    }

    void FinalizeMeshlet(
        MeshletBuildState& state,
        const std::vector<glm::vec3>& positions,
        std::vector<Meshlet>& meshlets,
        std::vector<std::uint32_t>& meshletVertices,
        std::vector<MeshletTriangle>& meshletTriangles)
    {
        if (state.triangles.empty() || state.localToGlobal.empty())
        {
            return;
        }

        glm::vec3 boundsMin(std::numeric_limits<float>::max());
        glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
        for (const std::uint32_t globalIndex : state.localToGlobal)
        {
            const glm::vec3& position = positions[globalIndex];
            boundsMin = glm::min(boundsMin, position);
            boundsMax = glm::max(boundsMax, position);
        }

        const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        float radius = 0.0f;
        for (const std::uint32_t globalIndex : state.localToGlobal)
        {
            radius = std::max(radius, glm::length(positions[globalIndex] - center));
        }

        state.meshlet.vertexOffset = static_cast<std::uint32_t>(meshletVertices.size());
        state.meshlet.vertexCount = static_cast<std::uint32_t>(state.localToGlobal.size());
        state.meshlet.triangleOffset = static_cast<std::uint32_t>(meshletTriangles.size());
        state.meshlet.triangleCount = static_cast<std::uint32_t>(state.triangles.size());
        state.meshlet.boundsCenter = center;
        state.meshlet.boundsRadius = radius;
        state.meshlet.boundsMin = boundsMin;
        state.meshlet.boundsMax = boundsMax;

        meshletVertices.insert(
            meshletVertices.end(),
            state.localToGlobal.begin(),
            state.localToGlobal.end());
        meshletTriangles.insert(
            meshletTriangles.end(),
            state.triangles.begin(),
            state.triangles.end());
        meshlets.push_back(state.meshlet);

        state = MeshletBuildState{};
    }
}

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
    BakeMeshlets();
}

void Mesh::EnsureGpuResources() const
{
    const bool meshletBuffersReady =
        m_meshlets.empty()
        || (m_meshletBuffer.IsValid()
            && m_meshletVertexBuffer.IsValid()
            && m_meshletTriangleBuffer.IsValid());
    if (m_vertexBuffer.IsValid()
        && m_vertexShaderResourceBuffer.IsValid()
        && m_indexBuffer.IsValid()
        && meshletBuffersReady)
    {
        return;
    }

    if (m_vertices.empty() || m_indices.empty() || m_indexCount == 0)
    {
        return;
    }

    if (!GfxContext::Get().IsInitialized() || GfxContext::Get().IsDeviceRemoved())
    {
        return;
    }

    Mesh* self = const_cast<Mesh*>(this);
    const std::uint32_t vertexByteSize =
        static_cast<std::uint32_t>(m_vertices.size() * sizeof(float));
    const std::uint32_t indexByteSize =
        static_cast<std::uint32_t>(m_indices.size() * sizeof(unsigned int));
    self->m_vertexBuffer.Create(GpuBuffer::Type::Vertex, m_vertices.data(), vertexByteSize);
    self->m_vertexShaderResourceBuffer.Create(
        GpuBuffer::Type::ShaderResource,
        m_vertices.data(),
        vertexByteSize);
    self->m_indexBuffer.Create(GpuBuffer::Type::Index, m_indices.data(), indexByteSize);

    if (!m_meshlets.empty() && !m_meshletVertices.empty() && !m_meshletTriangles.empty())
    {
        const std::uint32_t meshletByteSize =
            static_cast<std::uint32_t>(m_meshlets.size() * sizeof(Meshlet));
        const std::uint32_t meshletVertexByteSize =
            static_cast<std::uint32_t>(m_meshletVertices.size() * sizeof(std::uint32_t));
        const std::uint32_t meshletTriangleByteSize =
            static_cast<std::uint32_t>(m_meshletTriangles.size() * sizeof(MeshletTriangle));
        self->m_meshletBuffer.Create(
            GpuBuffer::Type::ShaderResource,
            m_meshlets.data(),
            meshletByteSize);
        self->m_meshletVertexBuffer.Create(
            GpuBuffer::Type::ShaderResource,
            m_meshletVertices.data(),
            meshletVertexByteSize);
        self->m_meshletTriangleBuffer.Create(
            GpuBuffer::Type::ShaderResource,
            m_meshletTriangles.data(),
            meshletTriangleByteSize);
    }
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
      m_meshlets(std::move(other.m_meshlets)),
      m_meshletVertices(std::move(other.m_meshletVertices)),
      m_meshletTriangles(std::move(other.m_meshletTriangles)),
      m_vertexBuffer(std::move(other.m_vertexBuffer)),
      m_vertexShaderResourceBuffer(std::move(other.m_vertexShaderResourceBuffer)),
      m_indexBuffer(std::move(other.m_indexBuffer)),
      m_meshletBuffer(std::move(other.m_meshletBuffer)),
      m_meshletVertexBuffer(std::move(other.m_meshletVertexBuffer)),
      m_meshletTriangleBuffer(std::move(other.m_meshletTriangleBuffer))
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
        m_meshlets = std::move(other.m_meshlets);
        m_meshletVertices = std::move(other.m_meshletVertices);
        m_meshletTriangles = std::move(other.m_meshletTriangles);
        m_vertexBuffer = std::move(other.m_vertexBuffer);
        m_vertexShaderResourceBuffer = std::move(other.m_vertexShaderResourceBuffer);
        m_indexBuffer = std::move(other.m_indexBuffer);
        m_meshletBuffer = std::move(other.m_meshletBuffer);
        m_meshletVertexBuffer = std::move(other.m_meshletVertexBuffer);
        m_meshletTriangleBuffer = std::move(other.m_meshletTriangleBuffer);
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
    EnsureGpuResources();

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

void Mesh::BakeMeshlets()
{
    m_meshlets.clear();
    m_meshletVertices.clear();
    m_meshletTriangles.clear();

    if (m_positions.empty() || m_indices.size() < 3)
    {
        return;
    }

    MeshletBuildState state;
    for (std::size_t triangleIndex = 0; triangleIndex + 2 < m_indices.size(); triangleIndex += 3)
    {
        const std::uint32_t i0 = m_indices[triangleIndex];
        const std::uint32_t i1 = m_indices[triangleIndex + 1];
        const std::uint32_t i2 = m_indices[triangleIndex + 2];
        if (i0 >= m_positions.size() || i1 >= m_positions.size() || i2 >= m_positions.size())
        {
            continue;
        }
        if (IsDegenerateTriangle(i0, i1, i2))
        {
            continue;
        }

        if (!CanAddTriangle(state, i0, i1, i2))
        {
            FinalizeMeshlet(state, m_positions, m_meshlets, m_meshletVertices, m_meshletTriangles);
        }

        AddTriangle(state, i0, i1, i2);
    }

    FinalizeMeshlet(state, m_positions, m_meshlets, m_meshletVertices, m_meshletTriangles);
}

void Mesh::ReleaseGpuResources()
{
    m_vertexBuffer.Destroy();
    m_vertexShaderResourceBuffer.Destroy();
    m_indexBuffer.Destroy();
    m_meshletBuffer.Destroy();
    m_meshletVertexBuffer.Destroy();
    m_meshletTriangleBuffer.Destroy();
    m_indexCount = 0;
}
