#pragma once

#include "app/SceneImportedMeshPool.h"

#include "engine/LightComponent.h"
#include "engine/Material.h"
#include "engine/SceneObjectId.h"
#include "engine/Transform.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Mesh;

struct ArchivedSelectionState
{
    std::vector<SceneObjectId> ids;
    SceneObjectId primary = kInvalidSceneObjectId;
};

struct ArchivedSceneObject
{
    int flatIndex = -1;
    SceneObjectId id = kInvalidSceneObjectId;
    std::string name;
    Mesh* mesh = nullptr;
    ImportMeshKey importedMeshKey;
    bool isImportedMesh = false;
    bool ownsImportedMesh = false;
    std::unique_ptr<Material> material;
    glm::vec3 localBoundsMin = glm::vec3(0.0f);
    glm::vec3 localBoundsMax = glm::vec3(0.0f);
    Transform transform;
    bool castShadow = true;
    bool receiveShadow = true;
    int siblingOrder = 0;
    std::optional<LightComponent> light;
};

struct SceneSubtreeArchive
{
    std::vector<ArchivedSceneObject> removedObjects;
    std::unordered_map<SceneObjectId, SceneObjectId> parentIdByObjectId;
    ImportedMeshReusePool importedMeshes;
    ArchivedSelectionState selectionBefore;
    ArchivedSelectionState selectionAfter;
    std::vector<SceneObjectId> removedRootIds;
};

class Scene;

ArchivedSelectionState CaptureArchivedSelection(const Scene& scene);
void ApplyArchivedSelection(Scene& scene, const ArchivedSelectionState& selection);
