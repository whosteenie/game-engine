#include "engine/assets/TangentSpace.h"

#include "engine/rendering/Mesh.h"

#include <mikktspace.h>

#include <cmath>
#include <stdexcept>

namespace
{
    struct MikkMeshData
    {
        std::vector<float>* vertices = nullptr;
        const std::vector<unsigned int>* indices = nullptr;
    };

    int GetNumFaces(const SMikkTSpaceContext* context)
    {
        const auto* mesh = static_cast<const MikkMeshData*>(context->m_pUserData);
        return static_cast<int>(mesh->indices->size() / 3);
    }

    int GetNumVerticesOfFace(const SMikkTSpaceContext* /*context*/, const int /*face*/)
    {
        return 3;
    }

    void GetPosition(const SMikkTSpaceContext* context, float positionOut[3], const int face, const int vert)
    {
        const auto* mesh = static_cast<const MikkMeshData*>(context->m_pUserData);
        const unsigned int vertexIndex = (*mesh->indices)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
        const float* vertex = &(*mesh->vertices)[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
        positionOut[0] = vertex[0];
        positionOut[1] = vertex[1];
        positionOut[2] = vertex[2];
    }

    void GetNormal(const SMikkTSpaceContext* context, float normalOut[3], const int face, const int vert)
    {
        const auto* mesh = static_cast<const MikkMeshData*>(context->m_pUserData);
        const unsigned int vertexIndex = (*mesh->indices)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
        const float* vertex = &(*mesh->vertices)[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
        normalOut[0] = vertex[3];
        normalOut[1] = vertex[4];
        normalOut[2] = vertex[5];
    }

    void GetTexCoord(const SMikkTSpaceContext* context, float texCoordOut[2], const int face, const int vert)
    {
        const auto* mesh = static_cast<const MikkMeshData*>(context->m_pUserData);
        const unsigned int vertexIndex = (*mesh->indices)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
        const float* vertex = &(*mesh->vertices)[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
        texCoordOut[0] = vertex[6];
        texCoordOut[1] = vertex[7];
    }

    void SetTSpaceBasic(
        const SMikkTSpaceContext* context,
        const float tangent[3],
        const float sign,
        const int face,
        const int vert)
    {
        auto* mesh = static_cast<MikkMeshData*>(context->m_pUserData);
        const unsigned int vertexIndex = (*mesh->indices)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vert)];
        float* vertex = &(*mesh->vertices)[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
        vertex[10] = tangent[0];
        vertex[11] = tangent[1];
        vertex[12] = tangent[2];
        vertex[13] = sign;
    }
}

namespace TangentSpace
{
    glm::vec4 FinalizeImportTangent(const glm::vec4& tangent)
    {
        glm::vec4 result = tangent;
        const float lengthSquared = glm::dot(glm::vec3(result), glm::vec3(result));
        if (lengthSquared > 1.0e-12f)
        {
            result = glm::vec4(glm::vec3(result) / std::sqrt(lengthSquared), result.w);
        }
        else
        {
            result = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        }

        if (result.w == 0.0f)
        {
            result.w = 1.0f;
        }

        return result;
    }

    void GenerateMikkTSpaceTangents(
        std::vector<float>& vertices,
        const std::vector<unsigned int>& indices)
    {
        if (vertices.empty() || indices.size() < 3 || indices.size() % 3 != 0)
        {
            return;
        }

        MikkMeshData meshData{&vertices, &indices};

        SMikkTSpaceInterface interface{};
        interface.m_getNumFaces = GetNumFaces;
        interface.m_getNumVerticesOfFace = GetNumVerticesOfFace;
        interface.m_getPosition = GetPosition;
        interface.m_getNormal = GetNormal;
        interface.m_getTexCoord = GetTexCoord;
        interface.m_setTSpaceBasic = SetTSpaceBasic;
        interface.m_setTSpace = nullptr;

        SMikkTSpaceContext context{};
        context.m_pInterface = &interface;
        context.m_pUserData = &meshData;

        if (!genTangSpaceDefault(&context))
        {
            throw std::runtime_error("MikkTSpace tangent generation failed");
        }
    }
}
