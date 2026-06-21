#include "engine/ModelImporter.h"

#include "engine/Constants.h"
#include "engine/Material.h"
#include "engine/Mesh.h"
#include "engine/Texture.h"
#include "engine/TextureCache.h"
#include "primitives/PrimitiveMeshUtils.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <stb_image.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <unordered_map>

namespace
{
    bool LoadImageData(
        tinygltf::Image* image,
        const int /*imageIndex*/,
        std::string* err,
        std::string* /*warn*/,
        int /*requestedWidth*/,
        int /*requestedHeight*/,
        const unsigned char* bytes,
        int size,
        void* /*userData*/)
    {
        int width = 0;
        int height = 0;
        int componentCount = 0;
        unsigned char* pixels = stbi_load_from_memory(bytes, size, &width, &height, &componentCount, 0);
        if (pixels == nullptr)
        {
            if (err != nullptr)
            {
                *err += "Failed to decode glTF image data.";
            }
            return false;
        }

        image->width = width;
        image->height = height;
        image->component = componentCount;
        image->image.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(componentCount));
        stbi_image_free(pixels);
        return true;
    }

    std::string GetFileExtensionLower(const std::string& path)
    {
        const std::filesystem::path filePath(path);
        std::string extension = filePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return extension;
    }

    std::string GetModelDirectory(const std::string& path)
    {
        return std::filesystem::path(path).parent_path().string();
    }

    std::string JoinPath(const std::string& directory, const std::string& relativePath)
    {
        return (std::filesystem::path(directory) / relativePath).string();
    }

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

        return reinterpret_cast<const T*>(
            &buffer.data[bufferView.byteOffset + accessor.byteOffset]);
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

    void GenerateTangents(
        std::vector<float>& vertices,
        const std::vector<unsigned int>& indices)
    {
        const unsigned int vertexCount = static_cast<unsigned int>(vertices.size() / Mesh::TexturedVertexFloatCount);
        std::vector<glm::vec3> tan1(vertexCount, glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vertexCount, glm::vec3(0.0f));

        for (std::size_t triangleIndex = 0; triangleIndex + 2 < indices.size(); triangleIndex += 3)
        {
            const unsigned int index0 = indices[triangleIndex];
            const unsigned int index1 = indices[triangleIndex + 1];
            const unsigned int index2 = indices[triangleIndex + 2];

            const float* vertex0 = &vertices[static_cast<std::size_t>(index0) * Mesh::TexturedVertexFloatCount];
            const float* vertex1 = &vertices[static_cast<std::size_t>(index1) * Mesh::TexturedVertexFloatCount];
            const float* vertex2 = &vertices[static_cast<std::size_t>(index2) * Mesh::TexturedVertexFloatCount];

            const glm::vec3 position0(vertex0[0], vertex0[1], vertex0[2]);
            const glm::vec3 position1(vertex1[0], vertex1[1], vertex1[2]);
            const glm::vec3 position2(vertex2[0], vertex2[1], vertex2[2]);
            const glm::vec2 uv0(vertex0[6], vertex0[7]);
            const glm::vec2 uv1(vertex1[6], vertex1[7]);
            const glm::vec2 uv2(vertex2[6], vertex2[7]);

            const glm::vec3 edge1 = position1 - position0;
            const glm::vec3 edge2 = position2 - position0;
            const glm::vec2 deltaUv1 = uv1 - uv0;
            const glm::vec2 deltaUv2 = uv2 - uv0;

            const float determinant = deltaUv1.x * deltaUv2.y - deltaUv2.x * deltaUv1.y;
            if (std::abs(determinant) < 1.0e-8f)
            {
                continue;
            }

            const float inverseDeterminant = 1.0f / determinant;
            const glm::vec3 tangent =
                (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * inverseDeterminant;
            const glm::vec3 bitangent =
                (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * inverseDeterminant;

            tan1[index0] += tangent;
            tan1[index1] += tangent;
            tan1[index2] += tangent;
            tan2[index0] += bitangent;
            tan2[index1] += bitangent;
            tan2[index2] += bitangent;
        }

        for (unsigned int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            float* vertex = &vertices[static_cast<std::size_t>(vertexIndex) * Mesh::TexturedVertexFloatCount];
            const glm::vec3 normal(vertex[3], vertex[4], vertex[5]);
            const glm::vec3 tangent = glm::normalize(tan1[vertexIndex] - normal * glm::dot(normal, tan1[vertexIndex]));
            vertex[8] = tangent.x;
            vertex[9] = tangent.y;
            vertex[10] = tangent.z;
        }
    }

    std::shared_ptr<Texture> LoadGltfImageTexture(
        const tinygltf::Model& model,
        int imageIndex,
        const std::string& modelDirectory,
        TextureColorSpace colorSpace,
        std::unordered_map<int, std::shared_ptr<Texture>>& imageCache)
    {
        if (imageIndex < 0)
        {
            return nullptr;
        }

        const auto cachedTexture = imageCache.find(imageIndex);
        if (cachedTexture != imageCache.end())
        {
            return cachedTexture->second;
        }

        const tinygltf::Image& image = model.images[static_cast<std::size_t>(imageIndex)];
        std::shared_ptr<Texture> texture;

        if (!image.uri.empty() && image.uri.rfind("data:", 0) != 0)
        {
            const std::string texturePath = JoinPath(modelDirectory, image.uri);
            try
            {
                texture = TextureCache::Get().Load(texturePath.c_str(), colorSpace);
            }
            catch (const std::exception&)
            {
                texture = nullptr;
            }
        }
        else if (!image.image.empty() && image.width > 0 && image.height > 0)
        {
            texture = Texture::CreateFromPixels(
                image.image.data(),
                image.width,
                image.height,
                image.component,
                colorSpace);
        }

        if (texture != nullptr && texture->IsValid())
        {
            imageCache.emplace(imageIndex, texture);
        }

        return texture;
    }

    std::shared_ptr<Texture> LoadGltfTexture(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory,
        TextureColorSpace colorSpace,
        std::unordered_map<int, std::shared_ptr<Texture>>& imageCache)
    {
        if (textureIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        return LoadGltfImageTexture(model, texture.source, modelDirectory, colorSpace, imageCache);
    }

    std::shared_ptr<Texture> CreateRoughnessMapFromMetallicRoughness(
        const tinygltf::Model& model,
        int textureIndex,
        const std::string& modelDirectory,
        std::unordered_map<int, std::shared_ptr<Texture>>& imageCache)
    {
        if (textureIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Texture& texture = model.textures[static_cast<std::size_t>(textureIndex)];
        const int imageIndex = texture.source;
        if (imageIndex < 0)
        {
            return nullptr;
        }

        const tinygltf::Image& image = model.images[static_cast<std::size_t>(imageIndex)];
        if (image.width <= 0 || image.height <= 0 || image.component < 1)
        {
            return LoadGltfTexture(model, textureIndex, modelDirectory, TextureColorSpace::Linear, imageCache);
        }

        std::vector<unsigned char> roughnessPixels(
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
        for (int pixelIndex = 0; pixelIndex < image.width * image.height; ++pixelIndex)
        {
            const int sourceIndex = pixelIndex * image.component;
            const unsigned char greenChannel = image.component > 1 ? image.image[static_cast<std::size_t>(sourceIndex + 1)] : image.image[static_cast<std::size_t>(sourceIndex)];
            roughnessPixels[static_cast<std::size_t>(pixelIndex)] = greenChannel;
        }

        return Texture::CreateFromPixels(
            roughnessPixels.data(),
            image.width,
            image.height,
            1,
            TextureColorSpace::Linear);
    }

    std::unique_ptr<Material> CreateMaterialFromGltf(
        const tinygltf::Model& model,
        int materialIndex,
        const std::string& modelDirectory,
        std::unordered_map<int, std::shared_ptr<Texture>>& imageCache)
    {
        glm::vec3 albedo(0.8f);
        float roughness = 0.5f;
        float metallic = 0.0f;

        if (materialIndex >= 0)
        {
            const tinygltf::Material& gltfMaterial = model.materials[static_cast<std::size_t>(materialIndex)];
            const auto& pbr = gltfMaterial.pbrMetallicRoughness;
            albedo = glm::vec3(
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2]));
            roughness = static_cast<float>(pbr.roughnessFactor);
            metallic = static_cast<float>(pbr.metallicFactor);

            auto material = std::make_unique<Material>(
                EngineConstants::LitVertexShader,
                EngineConstants::PbrFragmentShader,
                albedo,
                roughness,
                metallic);

            if (pbr.baseColorTexture.index >= 0)
            {
                material->SetAlbedoMap(LoadGltfTexture(
                    model,
                    pbr.baseColorTexture.index,
                    modelDirectory,
                    TextureColorSpace::SRGB,
                    imageCache));
            }

            if (pbr.metallicRoughnessTexture.index >= 0)
            {
                material->SetRoughnessMap(CreateRoughnessMapFromMetallicRoughness(
                    model,
                    pbr.metallicRoughnessTexture.index,
                    modelDirectory,
                    imageCache));
            }

            if (gltfMaterial.normalTexture.index >= 0)
            {
                material->SetNormalMap(LoadGltfTexture(
                    model,
                    gltfMaterial.normalTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    imageCache));
            }

            if (gltfMaterial.occlusionTexture.index >= 0)
            {
                material->SetAoMap(LoadGltfTexture(
                    model,
                    gltfMaterial.occlusionTexture.index,
                    modelDirectory,
                    TextureColorSpace::Linear,
                    imageCache));
            }

            return material;
        }

        return std::make_unique<Material>(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            albedo,
            roughness,
            metallic);
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

    bool BuildMeshFromPrimitive(
        const tinygltf::Model& model,
        const tinygltf::Primitive& primitive,
        std::unique_ptr<Mesh>& outMesh,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax)
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

            glm::vec2 uv(0.0f);
            if (texCoords != nullptr && vertexIndex < texCoordCount)
            {
                uv = ReadVec2(texCoords, vertexIndex);
            }

            glm::vec3 tangent(1.0f, 0.0f, 0.0f);
            if (tangents != nullptr && vertexIndex < tangentCount && tangentComponentCount >= 3)
            {
                tangent = glm::normalize(glm::vec3(
                    tangents[vertexIndex * tangentComponentCount],
                    tangents[vertexIndex * tangentComponentCount + 1],
                    tangents[vertexIndex * tangentComponentCount + 2]));
            }

            PrimitiveMesh::PushVertex(vertices, position, normal, uv, tangent);
            ExpandBounds(boundsMin, boundsMax, position);
        }

        if (tangents == nullptr)
        {
            GenerateTangents(vertices, indices);
        }

        outMesh = PrimitiveMesh::BuildMesh(vertices, indices);
        return outMesh != nullptr;
    }

    void VisitNode(
        const tinygltf::Model& model,
        int nodeIndex,
        int parentNodeIndex,
        const std::string& modelDirectory,
        std::unordered_map<int, std::shared_ptr<Texture>>& imageCache,
        std::vector<ImportedSceneNode>& nodes,
        int& nameCounter)
    {
        const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];

        ImportedSceneNode nodeObject;
        nodeObject.parentIndex = parentNodeIndex;
        nodeObject.transform = TransformFromMatrix(GetLocalNodeMatrix(node));
        if (!node.name.empty())
        {
            nodeObject.name = node.name;
        }
        else
        {
            nodeObject.name = "Node " + std::to_string(nameCounter++);
        }

        const int nodeObjectIndex = static_cast<int>(nodes.size());
        nodes.push_back(std::move(nodeObject));

        if (node.mesh >= 0)
        {
            const tinygltf::Mesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
            const bool singlePrimitive = mesh.primitives.size() == 1;

            for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
            {
                const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
                std::unique_ptr<Mesh> meshData;
                glm::vec3 boundsMin;
                glm::vec3 boundsMax;
                if (!BuildMeshFromPrimitive(model, primitive, meshData, boundsMin, boundsMax))
                {
                    continue;
                }

                auto material = CreateMaterialFromGltf(
                    model,
                    primitive.material,
                    modelDirectory,
                    imageCache);

                if (singlePrimitive)
                {
                    ImportedSceneNode& targetNode = nodes[static_cast<std::size_t>(nodeObjectIndex)];
                    targetNode.mesh = std::move(meshData);
                    targetNode.material = std::move(material);
                    targetNode.boundsMin = boundsMin;
                    targetNode.boundsMax = boundsMax;
                    targetNode.hasMesh = true;
                }
                else
                {
                    ImportedSceneNode meshChild;
                    meshChild.parentIndex = nodeObjectIndex;
                    meshChild.name = nodes[static_cast<std::size_t>(nodeObjectIndex)].name + " (" + std::to_string(primitiveIndex + 1) + ")";
                    meshChild.mesh = std::move(meshData);
                    meshChild.material = std::move(material);
                    meshChild.boundsMin = boundsMin;
                    meshChild.boundsMax = boundsMax;
                    meshChild.hasMesh = true;
                    nodes.push_back(std::move(meshChild));
                }
            }
        }

        for (int childIndex : node.children)
        {
            VisitNode(
                model,
                childIndex,
                nodeObjectIndex,
                modelDirectory,
                imageCache,
                nodes,
                nameCounter);
        }
    }

}

glm::mat4 GetImportedNodeWorldMatrix(
    const std::vector<ImportedSceneNode>& nodes,
    int nodeIndex)
{
    const ImportedSceneNode& node = nodes[static_cast<std::size_t>(nodeIndex)];
    const glm::mat4 localMatrix = node.transform.ToMatrix();
    if (node.parentIndex < 0)
    {
        return localMatrix;
    }

    return GetImportedNodeWorldMatrix(nodes, node.parentIndex) * localMatrix;
}

ImportedModel LoadModelFromFile(const std::string& path)
{
    ImportedModel importedModel;
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(LoadImageData, nullptr);

    tinygltf::Model model;
    std::string error;
    std::string warning;

    const std::string extension = GetFileExtensionLower(path);
    const bool loaded = extension == ".glb"
        ? loader.LoadBinaryFromFile(&model, &error, &warning, path)
        : loader.LoadASCIIFromFile(&model, &error, &warning, path);

    if (!warning.empty())
    {
        // Warnings are non-fatal; keep going.
    }

    if (!loaded)
    {
        importedModel.errorMessage = error.empty() ? "Failed to load model file." : error;
        return importedModel;
    }

    const std::string modelDirectory = GetModelDirectory(path);
    std::unordered_map<int, std::shared_ptr<Texture>> imageCache;
    int nameCounter = 1;

    ImportedSceneNode importRoot;
    importRoot.name = std::filesystem::path(path).stem().string();
    if (importRoot.name.empty())
    {
        importRoot.name = "Imported Model";
    }
    importRoot.parentIndex = -1;
    importedModel.rootNodeIndex = 0;
    importedModel.nodes.push_back(std::move(importRoot));

    if (!model.scenes.empty())
    {
        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
        const tinygltf::Scene& scene = model.scenes[static_cast<std::size_t>(sceneIndex)];
        for (int nodeIndex : scene.nodes)
        {
            VisitNode(
                model,
                nodeIndex,
                importedModel.rootNodeIndex,
                modelDirectory,
                imageCache,
                importedModel.nodes,
                nameCounter);
        }
    }
    else
    {
        for (int nodeIndex = 0; nodeIndex < static_cast<int>(model.nodes.size()); ++nodeIndex)
        {
            VisitNode(
                model,
                nodeIndex,
                importedModel.rootNodeIndex,
                modelDirectory,
                imageCache,
                importedModel.nodes,
                nameCounter);
        }
    }

    if (importedModel.nodes.size() <= 1)
    {
        importedModel.errorMessage = "No supported triangle meshes were found in the model.";
        importedModel.nodes.clear();
        importedModel.rootNodeIndex = -1;
        return importedModel;
    }

    return importedModel;
}
