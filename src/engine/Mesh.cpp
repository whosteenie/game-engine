#include <glad/glad.h>

#include "engine/Mesh.h"

#include <cmath>
#include <limits>

namespace
{
    constexpr float kRayEpsilon = 1e-7f;

    bool IntersectRayTriangle(
        const glm::vec3& origin,
        const glm::vec3& direction,
        const glm::vec3& v0,
        const glm::vec3& v1,
        const glm::vec3& v2,
        float& hitDistance)
    {
        const glm::vec3 edge1 = v1 - v0;
        const glm::vec3 edge2 = v2 - v0;
        const glm::vec3 pvec = glm::cross(direction, edge2);
        const float determinant = glm::dot(edge1, pvec);
        if (std::abs(determinant) < kRayEpsilon)
        {
            return false;
        }

        const float inverseDeterminant = 1.0f / determinant;
        const glm::vec3 tvec = origin - v0;
        const float u = glm::dot(tvec, pvec) * inverseDeterminant;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }

        const glm::vec3 qvec = glm::cross(tvec, edge1);
        const float v = glm::dot(direction, qvec) * inverseDeterminant;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }

        const float distance = glm::dot(edge2, qvec) * inverseDeterminant;
        if (distance < kRayEpsilon)
        {
            return false;
        }

        hitDistance = distance;
        return true;
    }
}

Mesh::Mesh(
    const float* vertices,
    unsigned int vertexCount,
    unsigned int floatsPerVertex,
    const unsigned int* indices,
    unsigned int indexCount)
{
    m_positions.reserve(vertexCount);
    for (unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const float* vertex = vertices + vertexIndex * floatsPerVertex;
        m_positions.emplace_back(vertex[0], vertex[1], vertex[2]);
    }

    m_indices.reserve(indexCount);
    for (unsigned int index = 0; index + 2 < indexCount; index += 3)
    {
        const unsigned int i0 = indices[index];
        const unsigned int i1 = indices[index + 1];
        const unsigned int i2 = indices[index + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            continue;
        }

        m_indices.push_back(i0);
        m_indices.push_back(i1);
        m_indices.push_back(i2);
    }

    m_indexCount = static_cast<unsigned int>(m_indices.size());

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        vertexCount * floatsPerVertex * sizeof(float),
        vertices,
        GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        m_indices.size() * sizeof(unsigned int),
        m_indices.data(),
        GL_STATIC_DRAW);

    const int stride = static_cast<int>(floatsPerVertex * sizeof(float));

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    if (floatsPerVertex >= TexturedVertexFloatCount)
    {
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8 * sizeof(float)));
        glEnableVertexAttribArray(3);

        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(10 * sizeof(float)));
        glEnableVertexAttribArray(4);
    }

    glBindVertexArray(0);
}

Mesh::~Mesh()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteBuffers(1, &m_ebo);
}

void Mesh::Draw() const
{
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
}

bool Mesh::IntersectRay(
    const glm::vec3& localOrigin,
    const glm::vec3& localDirection,
    float& hitDistance) const
{
    if (m_indices.size() < 3)
    {
        return false;
    }

    float closestDistance = std::numeric_limits<float>::max();
    bool hit = false;

    for (std::size_t index = 0; index + 2 < m_indices.size(); index += 3)
    {
        const unsigned int i0 = m_indices[index];
        const unsigned int i1 = m_indices[index + 1];
        const unsigned int i2 = m_indices[index + 2];
        if (i0 >= m_positions.size() || i1 >= m_positions.size() || i2 >= m_positions.size())
        {
            continue;
        }

        const glm::vec3& v0 = m_positions[i0];
        const glm::vec3& v1 = m_positions[i1];
        const glm::vec3& v2 = m_positions[i2];

        float triangleDistance = 0.0f;
        if (!IntersectRayTriangle(localOrigin, localDirection, v0, v1, v2, triangleDistance))
        {
            continue;
        }

        if (triangleDistance < closestDistance)
        {
            closestDistance = triangleDistance;
            hit = true;
        }
    }

    if (!hit)
    {
        return false;
    }

    hitDistance = closestDistance;
    return true;
}
