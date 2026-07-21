#pragma once

#include "engine/scene/Transform.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Material;
class Mesh;
class Scene;
struct ImportedModel;

class SceneImportService
{
public:
    std::vector<int> ImportModel(
        Scene& scene,
        const std::string& path,
        int parentIndex,
        const std::string& projectRoot,
        bool isProjectAsset = false);

    const std::string& GetLastImportError() const;
    const std::string& GetLastImportWarning() const;
    int PrewarmProjectModels(
        Scene& scene,
        const std::string& projectRoot,
        float progressStart = 0.0f,
        float progressEnd = 1.0f);
    void CacheLoadedProjectModel(
        Scene& scene,
        const std::string& importPath,
        ImportedModel&& geometryModel);
    void ClearMessages();
    void ClearCache();

private:
    struct CachedImportedNode
    {
        std::string name;
        int parentIndex = -1;
        Transform transform;
        Mesh* mesh = nullptr;
        std::unique_ptr<Material> material;
        glm::vec3 boundsMin = glm::vec3(0.0f);
        glm::vec3 boundsMax = glm::vec3(0.0f);
        bool hasMesh = false;
    };

    struct CachedImportedModel
    {
        std::vector<CachedImportedNode> nodes;
        int rootNodeIndex = -1;
        std::string warningMessage;
        int textureLoadFailures = 0;
        int texturesCached = 0;
    };

    std::vector<int> InstantiateCachedModel(
        Scene& scene,
        const std::string& importPath,
        const CachedImportedModel& cachedModel,
        int parentIndex);
    CachedImportedModel BuildCachedModel(Scene& scene, ImportedModel& importedModel);
    CachedImportedModel BuildCachedModelFromLoadedGeometry(
        Scene& scene,
        const std::string& importPath,
        ImportedModel&& geometryModel);
    bool IsCachedModelUsable(const Scene& scene, const CachedImportedModel& cachedModel) const;

    std::string m_lastImportError;
    std::string m_lastImportWarning;
    std::unordered_map<std::string, CachedImportedModel> m_cachedModels;
    std::unordered_map<std::string, std::string> m_sourceAssetAliases;
};
