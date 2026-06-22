#pragma once

#include "engine/Transform.h"

#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Material;
class Mesh;

struct ImportedSceneNode
{
    std::string name;
    int parentIndex = -1;
    Transform transform;
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<Material> material;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    bool hasMesh = false;
};

struct ImportedModel
{
    std::vector<ImportedSceneNode> nodes;
    int rootNodeIndex = -1;
    std::string errorMessage;
    std::string warningMessage;
};

using ModelOperationProgressFn = std::function<void(float progress, const std::string& message)>;

enum class ModelLoadMode
{
    Full,
    GeometryOnly,
};

ImportedModel LoadModelFromFile(
    const std::string& path,
    const std::string& projectRoot = {},
    ModelOperationProgressFn onProgress = {},
    ModelLoadMode loadMode = ModelLoadMode::Full);
bool EnsureGltfEmbeddedImagesOnDisk(
    const std::string& modelPath,
    std::string& outError,
    ModelOperationProgressFn onProgress = {});
glm::mat4 GetImportedNodeWorldMatrix(const std::vector<ImportedSceneNode>& nodes, int nodeIndex);
