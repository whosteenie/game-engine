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
    void VisitNode(
        const tinygltf::Model& model,
        int nodeIndex,
        int parentNodeIndex,
        const std::string& modelDirectory,
        const std::string& projectRoot,
        std::unordered_map<std::string, std::shared_ptr<Texture>>& textureCache,
        std::vector<ImportedSceneNode>& nodes,
        int& nameCounter,
        int totalNodes,
        int& processedNodes,
        float nodeProgressStart,
        ModelLoadMode loadMode,
        const ModelOperationProgressFn& onProgress)
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
                bool builtMesh = false;
                {
                    ProjectLoadBenchmark::ScopedPhase meshBuildPhase(
                        loadMode == ModelLoadMode::GeometryOnly
                            ? "project.deserialize.imported_model_mesh_build"
                            : nullptr);
                    builtMesh = BuildMesh(
                        model,
                        primitive,
                        meshData,
                        boundsMin,
                        boundsMax,
                        loadMode == ModelLoadMode::GeometryOnly);
                }
                if (!builtMesh)
                {
                    continue;
                }

                std::unique_ptr<Material> material;
                if (loadMode == ModelLoadMode::Full)
                {
                    material = CreateMaterial(
                        model,
                        primitive.material,
                        modelDirectory,
                        projectRoot,
                        textureCache);
                }

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
                projectRoot,
                textureCache,
                nodes,
                nameCounter,
                totalNodes,
                processedNodes,
                nodeProgressStart,
                loadMode,
                onProgress);
        }

        ++processedNodes;
        if (onProgress && totalNodes > 0)
        {
            const float nodeProgressSpan = 1.0f - nodeProgressStart;
            const float progress = nodeProgressStart
                + (nodeProgressSpan * static_cast<float>(processedNodes) / static_cast<float>(totalNodes));
            onProgress(progress, nodes[static_cast<std::size_t>(nodeObjectIndex)].name);
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

