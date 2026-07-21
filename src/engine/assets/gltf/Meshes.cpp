#include "engine/assets/gltf/Detail.h"

#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/assets/TangentSpace.h"
#include "engine/assets/ProjectAssets.h"
#include "engine/rendering/resources/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/rendering/resources/TextureSamplerSettings.h"
#include "primitives/PrimitiveMeshUtils.h"
#include "engine/platform/diagnostics/RenderPathDiagnostics.h"

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace GltfDetail
{
    template<typename T>
    const T* GetAccessorData(
        const tinygltf::Model& model,
        int accessorIndex,
        int& componentCount,
        int& accessorCount)
    {
        if (accessorIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessorIndex)];
        const tinygltf::BufferView& bufferView = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
        const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(bufferView.buffer)];

        componentCount = tinygltf::GetNumComponentsInType(accessor.type);
        accessorCount = static_cast<int>(accessor.count);
        return reinterpret_cast<const T*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
    }

    glm::vec3 ReadVec3(const float* data, int index)
    {
        const int offset = index * 3;
        return glm::vec3(data[offset], data[offset + 1], data[offset + 2]);
    }

    glm::vec2 ReadVec2(const float* data, int index)
    {
        const int offset = index * 2;
        return glm::vec2(data[offset], data[offset + 1]);
    }

    void ExpandBounds(glm::vec3& boundsMin, glm::vec3& boundsMax, const glm::vec3& point)
    {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }

    Transform TransformFromMatrix(const glm::mat4& matrix)
    {
        Transform transform;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(matrix, transform.scale, transform.rotation, transform.position, skew, perspective);
        return transform;
    }

    glm::mat4 GetLocalNodeMatrix(const tinygltf::Node& node)
    {
        if (!node.matrix.empty())
        {
            glm::mat4 matrix(1.0f);
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    matrix[column][row] = static_cast<float>(node.matrix[static_cast<std::size_t>(column * 4 + row)]);
                }
            }
            return matrix;
        }

        glm::mat4 matrix(1.0f);
        if (!node.translation.empty())
        {
            matrix = glm::translate(
                matrix,
                glm::vec3(
                    static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2])));
        }

        if (!node.rotation.empty())
        {
            const glm::quat rotation(
                static_cast<float>(node.rotation[3]),
                static_cast<float>(node.rotation[0]),
                static_cast<float>(node.rotation[1]),
                static_cast<float>(node.rotation[2]));
            matrix *= glm::mat4_cast(rotation);
        }

        if (!node.scale.empty())
        {
            matrix = glm::scale(
                matrix,
                glm::vec3(
                    static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]),
                    static_cast<float>(node.scale[2])));
        }

        return matrix;
    }

    bool BuildMesh(
        const tinygltf::Model& model,
        const tinygltf::Primitive& primitive,
        std::unique_ptr<Mesh>& outMesh,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax,
        const bool benchmarkProjectGeometryLoad)
    {
        if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1)
        {
            return false;
        }

        const auto positionIt = primitive.attributes.find("POSITION");
        if (positionIt == primitive.attributes.end())
        {
            return false;
        }

        int positionComponentCount = 0;
        int positionCount = 0;
        const float* positions = GetAccessorData<float>(model, positionIt->second, positionComponentCount, positionCount);
        if (positions == nullptr || positionComponentCount < 3 || positionCount <= 0)
        {
            return false;
        }

        int normalComponentCount = 0;
        int normalCount = 0;
        const float* normals = nullptr;
        const auto normalIt = primitive.attributes.find("NORMAL");
        if (normalIt != primitive.attributes.end())
        {
            normals = GetAccessorData<float>(model, normalIt->second, normalComponentCount, normalCount);
        }

        int texCoordComponentCount = 0;
        int texCoordCount = 0;
        const float* texCoords = nullptr;
        const auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
        if (texCoordIt != primitive.attributes.end())
        {
            texCoords = GetAccessorData<float>(model, texCoordIt->second, texCoordComponentCount, texCoordCount);
        }

        int texCoord1ComponentCount = 0;
        int texCoord1Count = 0;
        const float* texCoords1 = nullptr;
        const auto texCoord1It = primitive.attributes.find("TEXCOORD_1");
        if (texCoord1It != primitive.attributes.end())
        {
            texCoords1 = GetAccessorData<float>(model, texCoord1It->second, texCoord1ComponentCount, texCoord1Count);
        }

        int tangentComponentCount = 0;
        int tangentCount = 0;
        const float* tangents = nullptr;
        const auto tangentIt = primitive.attributes.find("TANGENT");
        if (tangentIt != primitive.attributes.end())
        {
            tangents = GetAccessorData<float>(model, tangentIt->second, tangentComponentCount, tangentCount);
        }

        std::vector<unsigned int> indices;
        if (primitive.indices >= 0)
        {
            int indexComponentCount = 0;
            int indexCount = 0;
            const tinygltf::Accessor& indexAccessor = model.accessors[static_cast<std::size_t>(primitive.indices)];
            const tinygltf::BufferView& indexBufferView = model.bufferViews[static_cast<std::size_t>(indexAccessor.bufferView)];
            const tinygltf::Buffer& indexBuffer = model.buffers[static_cast<std::size_t>(indexBufferView.buffer)];
            const unsigned char* indexData =
                &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];
            indexComponentCount = tinygltf::GetNumComponentsInType(indexAccessor.type);
            indexCount = static_cast<int>(indexAccessor.count);

            indices.reserve(static_cast<std::size_t>(indexCount));
            for (int index = 0; index < indexCount; ++index)
            {
                unsigned int vertexIndex = 0;
                const std::size_t byteOffset = static_cast<std::size_t>(index) * static_cast<std::size_t>(indexAccessor.ByteStride(indexBufferView));
                switch (indexAccessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    vertexIndex = indexData[byteOffset];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    vertexIndex = *reinterpret_cast<const unsigned short*>(&indexData[byteOffset]);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    vertexIndex = *reinterpret_cast<const unsigned int*>(&indexData[byteOffset]);
                    break;
                default:
                    return false;
                }

                indices.push_back(vertexIndex);
            }
        }
        else
        {
            indices.reserve(static_cast<std::size_t>(positionCount));
            for (int index = 0; index < positionCount; ++index)
            {
                indices.push_back(static_cast<unsigned int>(index));
            }
        }

        boundsMin = glm::vec3(std::numeric_limits<float>::max());
        boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

        std::vector<float> vertices;
        vertices.reserve(static_cast<std::size_t>(positionCount) * Mesh::TexturedVertexFloatCount);

        for (int vertexIndex = 0; vertexIndex < positionCount; ++vertexIndex)
        {
            const glm::vec3 position = ReadVec3(positions, vertexIndex);
            glm::vec3 normal(0.0f, 1.0f, 0.0f);
            if (normals != nullptr && vertexIndex < normalCount)
            {
                normal = glm::normalize(ReadVec3(normals, vertexIndex));
            }

            glm::vec2 uv0(0.0f);
            if (texCoords != nullptr && vertexIndex < texCoordCount)
            {
                uv0 = ReadVec2(texCoords, vertexIndex);
            }

            glm::vec2 uv1(0.0f);
            if (texCoords1 != nullptr && vertexIndex < texCoord1Count)
            {
                uv1 = ReadVec2(texCoords1, vertexIndex);
            }

            glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            if (tangents != nullptr && vertexIndex < tangentCount && tangentComponentCount >= 3)
            {
                const int tangentOffset = vertexIndex * tangentComponentCount;
                const float handedness = tangentComponentCount >= 4 ? tangents[tangentOffset + 3] : 1.0f;
                const glm::vec3 tangentDirection = glm::normalize(glm::vec3(
                    tangents[tangentOffset],
                    tangents[tangentOffset + 1],
                    tangents[tangentOffset + 2]));
                tangent = glm::vec4(tangentDirection, handedness);
            }

            PrimitiveMesh::PushVertex(vertices, position, normal, uv0, tangent, uv1, texCoords1 != nullptr);
            ExpandBounds(boundsMin, boundsMax, position);
        }

        if (tangents == nullptr)
        {
            ProjectLoadBenchmark::ScopedPhase tangentGenerationPhase(
                benchmarkProjectGeometryLoad
                    ? "project.deserialize.imported_model_tangent_generation"
                    : nullptr);
            TangentSpace::GenerateMikkTSpaceTangents(vertices, indices);
        }

        for (std::size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex += Mesh::TexturedVertexFloatCount)
        {
            const glm::vec4 tangent(
                vertices[vertexIndex + 10],
                vertices[vertexIndex + 11],
                vertices[vertexIndex + 12],
                vertices[vertexIndex + 13]);
            const glm::vec4 finalized = TangentSpace::FinalizeImportTangent(tangent);
            vertices[vertexIndex + 10] = finalized.x;
            vertices[vertexIndex + 11] = finalized.y;
            vertices[vertexIndex + 12] = finalized.z;
            vertices[vertexIndex + 13] = finalized.w;
        }

        outMesh = PrimitiveMesh::BuildMesh(vertices, indices);
        return outMesh != nullptr;
    }

}
