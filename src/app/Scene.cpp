#include <glad/glad.h>

#include "app/Scene.h"

#include "app/SceneEditor.h"
#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/MaterialTextures.h"
#include "engine/Mesh.h"
#include "engine/ModelImporter.h"
#include "engine/NativeProgressWindow.h"
#include "engine/ProjectAssets.h"
#include "engine/SceneHierarchy.h"
#include "primitives/Cube.h"
#include "primitives/Sphere.h"
#include "primitives/Cylinder.h"
#include "primitives/Capsule.h"
#include "primitives/Plane.h"
#include "engine/GridRenderer.h"
#include "engine/LightComponent.h"
#include "engine/LightGizmoRenderer.h"
#include "engine/SceneLighting.h"
#include "engine/ScreenSpaceEffects.h"
#include "engine/ShadowMap.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <limits>
#include <unordered_map>
#include <unordered_set>

Scene::Scene()
    : m_cubeMesh(CreateCubeMesh()),
      m_sphereMesh(CreateSphereMesh()),
      m_cylinderMesh(CreateCylinderMesh()),
      m_capsuleMesh(CreateCapsuleMesh()),
      m_planeMesh(CreatePlaneMesh(FloorHalfExtent)),
      m_grid(std::make_unique<GridRenderer>()),
      m_lightGizmos(std::make_unique<LightGizmoRenderer>()),
      m_sceneEditor(std::make_unique<SceneEditor>()),
      m_shadowMap(std::make_unique<ShadowMap>()),
      m_ibl(std::make_unique<IBL>(EngineConstants::EnvironmentHdr)),
      m_screenSpaceEffects(std::make_unique<ScreenSpaceEffects>()),
      m_shadowDepthShader(std::make_unique<Shader>(
          EngineConstants::ShadowDepthVertexShader,
          EngineConstants::ShadowDepthFragmentShader))
{
    SetupDefaultSunLight();
}

Scene::~Scene() = default;

void Scene::SetupDefaultSunLight()
{
    LightComponent sunLight = MakeDefaultLightComponent(LightType::Directional);
    Transform sunTransform = MakeDefaultLightTransform(LightType::Directional);

    m_objects.emplace_back(
        "Sun",
        nullptr,
        nullptr,
        glm::vec3(-0.15f),
        glm::vec3(0.15f),
        sunTransform,
        true,
        false,
        false,
        -1,
        0,
        std::move(sunLight));
}

void Scene::SyncLighting() const
{
    m_lighting.ClearLights();
    int shadowLightIndex = -1;

    for (std::size_t objectIndex = 0; objectIndex < m_objects.size(); ++objectIndex)
    {
        const SceneObject& object = m_objects[objectIndex];
        if (!object.HasLight())
        {
            continue;
        }

        if (m_lighting.GetLightCount() >= static_cast<std::size_t>(SceneLighting::MaxLights))
        {
            break;
        }

        const int lightSlot = static_cast<int>(m_lighting.GetLightCount());
        m_lighting.AddLight(BuildLightFromSceneObject(object, GetWorldMatrix(static_cast<int>(objectIndex))));

        if (shadowLightIndex < 0 && object.GetLight().castsShadow)
        {
            shadowLightIndex = lightSlot;
        }
    }

    m_lighting.SetShadowLightIndex(shadowLightIndex);
}

void Scene::SetupObjects()
{
    auto floorMaterial = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        glm::vec3(1.0f),
        1.0f,
        0.0f);
    ApplyConcreteFloorMaterialMaps(*floorMaterial);

    m_objects.emplace_back(
        "Floor",
        m_planeMesh.get(),
        std::move(floorMaterial),
        glm::vec3(-FloorHalfExtent, -0.01f, -FloorHalfExtent),
        glm::vec3(FloorHalfExtent, 0.01f, FloorHalfExtent),
        Transform{},
        true,
        true,
        true,
        -1,
        1);

    auto cubeMaterial = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        glm::vec3(1.0f),
        0.85f,
        0.0f);
    ApplyWoodTableMaterialMaps(*cubeMaterial);

    Transform cubeTransform;
    cubeTransform.position = glm::vec3(0.0f, 1.5f, 0.0f);

    m_objects.emplace_back(
        "Cube",
        m_cubeMesh.get(),
        std::move(cubeMaterial),
        glm::vec3(-0.5f),
        glm::vec3(0.5f),
        cubeTransform,
        true,
        true,
        true,
        -1,
        2);

    m_selectedObjectIndex = 2;
}

namespace
{
    struct PrimitiveSpawnInfo
    {
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        glm::vec3 position;
        bool movable = true;
        bool castShadow = true;
        bool receiveShadow = true;
    };

    PrimitiveSpawnInfo GetPrimitiveSpawnInfo(ScenePrimitive primitive, int instanceNumber)
    {
        const float spread = static_cast<float>(instanceNumber) * 1.25f;

        switch (primitive)
        {
        case ScenePrimitive::Cube:
            return {
                glm::vec3(-0.5f),
                glm::vec3(0.5f),
                glm::vec3(spread, 1.5f, 0.0f),
            };
        case ScenePrimitive::Sphere:
            return {
                glm::vec3(-0.5f),
                glm::vec3(0.5f),
                glm::vec3(spread, 1.5f, 1.5f),
            };
        case ScenePrimitive::Cylinder:
            return {
                glm::vec3(-0.5f),
                glm::vec3(0.5f),
                glm::vec3(spread, 1.5f, -1.5f),
            };
        case ScenePrimitive::Capsule:
            return {
                glm::vec3(-0.5f, -1.0f, -0.5f),
                glm::vec3(0.5f, 1.0f, 0.5f),
                glm::vec3(spread, 1.0f, 3.0f),
            };
        case ScenePrimitive::Plane:
            return {
                glm::vec3(-Scene::FloorHalfExtent, -0.01f, -Scene::FloorHalfExtent),
                glm::vec3(Scene::FloorHalfExtent, 0.01f, Scene::FloorHalfExtent),
                glm::vec3(spread, 0.0f, -3.0f),
            };
        }

        return {
            glm::vec3(-0.5f),
            glm::vec3(0.5f),
            glm::vec3(spread, 1.5f, 0.0f),
        };
    }

    std::unique_ptr<Material> CreateDefaultObjectMaterial()
    {
        return std::make_unique<Material>(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            glm::vec3(0.78f, 0.78f, 0.78f),
            0.55f,
            0.0f);
    }
}

Mesh* Scene::GetMeshForPrimitive(ScenePrimitive primitive) const
{
    switch (primitive)
    {
    case ScenePrimitive::Cube:
        return m_cubeMesh.get();
    case ScenePrimitive::Sphere:
        return m_sphereMesh.get();
    case ScenePrimitive::Cylinder:
        return m_cylinderMesh.get();
    case ScenePrimitive::Capsule:
        return m_capsuleMesh.get();
    case ScenePrimitive::Plane:
        return m_planeMesh.get();
    }

    return m_cubeMesh.get();
}

Mesh* Scene::AdoptImportedMesh(std::unique_ptr<Mesh> mesh)
{
    m_importedMeshes.push_back(std::move(mesh));
    return m_importedMeshes.back().get();
}

int Scene::GetNextObjectNumber(ScenePrimitive primitive)
{
    switch (primitive)
    {
    case ScenePrimitive::Cube:
        return m_nextCubeNumber++;
    case ScenePrimitive::Sphere:
        return m_nextSphereNumber++;
    case ScenePrimitive::Cylinder:
        return m_nextCylinderNumber++;
    case ScenePrimitive::Capsule:
        return m_nextCapsuleNumber++;
    case ScenePrimitive::Plane:
        return m_nextPlaneNumber++;
    }

    return 1;
}

int Scene::AddObject(ScenePrimitive primitive, int parentIndex)
{
    const int instanceNumber = GetNextObjectNumber(primitive);
    const PrimitiveSpawnInfo spawnInfo = GetPrimitiveSpawnInfo(primitive, instanceNumber);

    const std::string objectName =
        std::string(GetScenePrimitiveDisplayName(primitive)) + " " + std::to_string(instanceNumber);

    Transform transform;
    if (parentIndex >= 0)
    {
        transform.position = glm::vec3(0.0f, 1.5f, 0.0f);
    }
    else
    {
        transform.position = spawnInfo.position;
    }

    m_objects.emplace_back(
        objectName,
        GetMeshForPrimitive(primitive),
        CreateDefaultObjectMaterial(),
        spawnInfo.boundsMin,
        spawnInfo.boundsMax,
        transform,
        spawnInfo.movable,
        spawnInfo.castShadow,
        spawnInfo.receiveShadow,
        parentIndex,
        AllocateSiblingOrder(parentIndex));

    MarkDirty();
    return static_cast<int>(m_objects.size()) - 1;
}

int Scene::AddEmptyObject(int parentIndex)
{
    const std::string objectName = "Empty " + std::to_string(m_nextEmptyNumber++);

    m_objects.emplace_back(
        objectName,
        nullptr,
        nullptr,
        glm::vec3(0.0f),
        glm::vec3(0.0f),
        Transform{},
        true,
        false,
        false,
        parentIndex,
        AllocateSiblingOrder(parentIndex));

    MarkDirty();
    return static_cast<int>(m_objects.size()) - 1;
}

int Scene::AddLightObject(LightType type, int parentIndex)
{
    LightComponent lightComponent = MakeDefaultLightComponent(type);
    Transform transform = MakeDefaultLightTransform(type);

    if (lightComponent.castsShadow)
    {
        for (const SceneObject& object : m_objects)
        {
            if (object.HasLight() && object.GetLight().castsShadow)
            {
                lightComponent.castsShadow = false;
                break;
            }
        }
    }

    std::string objectName;
    switch (type)
    {
    case LightType::Directional:
        objectName = "Directional Light " + std::to_string(m_nextDirectionalLightNumber++);
        break;
    case LightType::Point:
        objectName = "Point Light " + std::to_string(m_nextPointLightNumber++);
        break;
    case LightType::Spot:
        objectName = "Spot Light " + std::to_string(m_nextSpotLightNumber++);
        break;
    }

    m_objects.emplace_back(
        objectName,
        nullptr,
        nullptr,
        glm::vec3(-0.15f),
        glm::vec3(0.15f),
        transform,
        true,
        false,
        false,
        parentIndex,
        AllocateSiblingOrder(parentIndex),
        std::move(lightComponent));

    MarkDirty();
    return static_cast<int>(m_objects.size()) - 1;
}

glm::mat4 Scene::GetWorldMatrix(int objectIndex) const
{
    return GetObjectWorldMatrix(m_objects, objectIndex);
}

glm::mat4 Scene::GetGizmoWorldMatrix(int objectIndex) const
{
    return GetObjectGizmoWorldMatrix(m_objects, objectIndex);
}

void Scene::GetLocalSelectionBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const
{
    GetObjectLocalSelectionBounds(m_objects, objectIndex, boundsMin, boundsMax);
}

void Scene::ApplyGizmoWorldMatrix(int objectIndex, const glm::mat4& gizmoWorldMatrix)
{
    ApplyObjectGizmoWorldMatrix(m_objects, objectIndex, gizmoWorldMatrix);
    MarkDirty();
}

void Scene::GetWorldBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const
{
    GetObjectWorldBounds(m_objects, objectIndex, boundsMin, boundsMax);
}

std::vector<int> Scene::GetChildren(int objectIndex) const
{
    return GetObjectChildren(m_objects, objectIndex);
}

std::vector<int> Scene::GetRootObjectIndices() const
{
    return ::GetRootObjectIndices(m_objects);
}

void Scene::CollectDescendantIndices(int objectIndex, std::vector<int>& outIndices) const
{
    outIndices.push_back(objectIndex);
    for (int childIndex : GetChildren(objectIndex))
    {
        CollectDescendantIndices(childIndex, outIndices);
    }
}

void Scene::RemapParentIndicesAfterRemoval(int removedIndex)
{
    for (SceneObject& object : m_objects)
    {
        int parentIndex = object.GetParentIndex();
        if (parentIndex > removedIndex)
        {
            object.SetParentIndex(parentIndex - 1);
        }
    }

    if (m_selectedObjectIndex > removedIndex)
    {
        --m_selectedObjectIndex;
    }
    else if (m_selectedObjectIndex == removedIndex)
    {
        m_selectedObjectIndex = -1;
    }
}

std::vector<int> Scene::ImportModel(const std::string& path, int parentIndex, const std::string& projectRoot)
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    const std::string modelName = std::filesystem::path(path).filename().string();
    ScopedNativeProgress progress("Importing Model", modelName);

    std::string importPath = path;
    if (!projectRoot.empty())
    {
        progress.SetMessage("Copying model into project...");
        const ImportModelAssetResult assetResult = ImportModelToProject(path, projectRoot);
        if (!assetResult.success)
        {
            m_lastImportError = assetResult.errorMessage.empty()
                ? "Failed to copy model into project assets."
                : assetResult.errorMessage;
            return {};
        }

        importPath = assetResult.absolutePath;
    }

    progress.SetMessage("Loading meshes and textures...");
    ImportedModel importedModel = LoadModelFromFile(
        importPath,
        projectRoot,
        [&](float loadProgress, const std::string& detail) {
            progress.SetProgress(loadProgress);
            if (!detail.empty())
            {
                progress.SetMessage("Loading meshes and textures — " + detail);
            }
        });
    if (!importedModel.errorMessage.empty())
    {
        m_lastImportError = importedModel.errorMessage;
        return {};
    }

    m_lastImportWarning = importedModel.warningMessage;

    if (importedModel.nodes.empty() || importedModel.rootNodeIndex < 0)
    {
        m_lastImportError = "No meshes were imported from the model.";
        return {};
    }

    const bool placeAtSceneRoot = parentIndex < 0;
    const float spread = placeAtSceneRoot ? static_cast<float>(m_nextImportNumber) * 2.5f : 0.0f;
    if (placeAtSceneRoot)
    {
        ++m_nextImportNumber;
    }

    float minWorldY = std::numeric_limits<float>::max();
    const glm::mat4 sceneParentWorld = parentIndex >= 0 ? GetWorldMatrix(parentIndex) : glm::mat4(1.0f);
    for (std::size_t nodeIndex = 0; nodeIndex < importedModel.nodes.size(); ++nodeIndex)
    {
        const ImportedSceneNode& node = importedModel.nodes[nodeIndex];
        if (!node.hasMesh)
        {
            continue;
        }

        glm::mat4 worldMatrix = GetImportedNodeWorldMatrix(importedModel.nodes, static_cast<int>(nodeIndex));
        if (parentIndex >= 0)
        {
            worldMatrix = sceneParentWorld * worldMatrix;
        }

        const glm::vec3 corners[2] = {node.boundsMin, node.boundsMax};
        for (const glm::vec3& corner : corners)
        {
            const glm::vec4 worldCorner = worldMatrix * glm::vec4(corner, 1.0f);
            minWorldY = std::min(minWorldY, worldCorner.y);
        }
    }

    const float floorOffset = minWorldY < std::numeric_limits<float>::max() ? -minWorldY : 0.0f;
    importedModel.nodes[static_cast<std::size_t>(importedModel.rootNodeIndex)].transform.position +=
        glm::vec3(spread, floorOffset, 0.0f);

    const int baseObjectIndex = static_cast<int>(m_objects.size());
    std::vector<int> importedSceneIndices;
    importedSceneIndices.reserve(importedModel.nodes.size());

    for (std::size_t importedNodeIndex = 0; importedNodeIndex < importedModel.nodes.size(); ++importedNodeIndex)
    {
        ImportedSceneNode& node = importedModel.nodes[importedNodeIndex];
        Mesh* mesh = nullptr;
        if (node.hasMesh && node.mesh != nullptr)
        {
            m_importedMeshes.push_back(std::move(node.mesh));
            mesh = m_importedMeshes.back().get();
        }

        int parentSceneIndex = -1;
        if (node.parentIndex >= 0)
        {
            parentSceneIndex = importedSceneIndices[static_cast<std::size_t>(node.parentIndex)];
        }
        else if (static_cast<int>(importedNodeIndex) == importedModel.rootNodeIndex && parentIndex >= 0)
        {
            parentSceneIndex = parentIndex;
        }

        m_objects.emplace_back(
            node.name,
            mesh,
            node.hasMesh ? std::move(node.material) : nullptr,
            node.hasMesh ? node.boundsMin : glm::vec3(0.0f),
            node.hasMesh ? node.boundsMax : glm::vec3(0.0f),
            node.transform,
            true,
            node.hasMesh,
            node.hasMesh,
            parentSceneIndex,
            AllocateSiblingOrder(parentSceneIndex));

        importedSceneIndices.push_back(static_cast<int>(m_objects.size()) - 1);

        SceneObject& createdObject = m_objects.back();
        createdObject.SetImportSource(importPath, static_cast<int>(importedNodeIndex));
    }

    MarkDirty();
    return {baseObjectIndex + importedModel.rootNodeIndex};
}

const std::string& Scene::GetLastImportError() const
{
    return m_lastImportError;
}

const std::string& Scene::GetLastImportWarning() const
{
    return m_lastImportWarning;
}

bool Scene::RemoveObject(std::size_t index)
{
    if (index >= m_objects.size())
    {
        return false;
    }

    std::vector<int> indicesToRemove;
    CollectDescendantIndices(static_cast<int>(index), indicesToRemove);
    std::sort(indicesToRemove.begin(), indicesToRemove.end(), std::greater<int>());

    const bool selectionRemoved = m_selectedObjectIndex >= 0
        && std::find(indicesToRemove.begin(), indicesToRemove.end(), m_selectedObjectIndex)
            != indicesToRemove.end();

    for (int removeIndex : indicesToRemove)
    {
        m_objects.erase(m_objects.begin() + removeIndex);
        RemapParentIndicesAfterRemoval(removeIndex);
    }

    if (m_objects.empty() || selectionRemoved)
    {
        m_selectedObjectIndex = -1;
    }
    else if (m_selectedObjectIndex >= static_cast<int>(m_objects.size()))
    {
        m_selectedObjectIndex = static_cast<int>(m_objects.size()) - 1;
    }

    PruneUnusedImportedMeshes();

    MarkDirty();
    return true;
}

std::string Scene::MakeDuplicateObjectName(const std::string& sourceName) const
{
    auto nameExists = [this](const std::string& name) {
        for (const SceneObject& object : m_objects)
        {
            if (object.GetName() == name)
            {
                return true;
            }
        }

        return false;
    };

    for (int suffix = 1; suffix < 1000; ++suffix)
    {
        const std::string candidate = sourceName + " (" + std::to_string(suffix) + ")";
        if (!nameExists(candidate))
        {
            return candidate;
        }
    }

    return sourceName + " (copy)";
}

int Scene::DuplicateObject(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(m_objects.size()))
    {
        return -1;
    }

    std::vector<int> sourceIndices;
    CollectDescendantIndices(objectIndex, sourceIndices);

    std::unordered_map<int, int> indexMap;
    int duplicateRootIndex = -1;

    for (int sourceIndex : sourceIndices)
    {
        const SceneObject& source = m_objects[static_cast<std::size_t>(sourceIndex)];

        int newParentIndex = -1;
        const int sourceParentIndex = source.GetParentIndex();
        if (sourceIndex == objectIndex)
        {
            newParentIndex = sourceParentIndex;
        }
        else if (sourceParentIndex >= 0)
        {
            newParentIndex = indexMap.at(sourceParentIndex);
        }

        std::unique_ptr<Material> materialClone;
        if (source.HasMaterial())
        {
            materialClone = source.GetMaterial().Clone();
        }

        std::string objectName = source.GetName();
        if (sourceIndex == objectIndex)
        {
            objectName = MakeDuplicateObjectName(source.GetName());
        }

        std::optional<LightComponent> lightClone;
        if (source.HasLight())
        {
            lightClone = source.GetLight();
        }

        m_objects.emplace_back(
            objectName,
            source.GetMesh(),
            std::move(materialClone),
            source.GetLocalBoundsMin(),
            source.GetLocalBoundsMax(),
            source.GetTransform(),
            source.IsMovable(),
            source.CastsShadow(),
            source.ReceivesShadow(),
            newParentIndex,
            source.GetSiblingOrder(),
            std::move(lightClone));

        const int newIndex = static_cast<int>(m_objects.size()) - 1;
        indexMap[sourceIndex] = newIndex;
        if (sourceIndex == objectIndex)
        {
            duplicateRootIndex = newIndex;
        }
    }

    if (duplicateRootIndex < 0)
    {
        return -1;
    }

    PlaceObjectInHierarchy(duplicateRootIndex, objectIndex, HierarchyInsertMode::After);
    return duplicateRootIndex;
}

bool Scene::CanReparentObject(int objectIndex, int newParentIndex) const
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(m_objects.size()))
    {
        return false;
    }

    if (newParentIndex < -1 || newParentIndex >= static_cast<int>(m_objects.size()))
    {
        return false;
    }

    if (objectIndex == newParentIndex)
    {
        return false;
    }

    if (newParentIndex >= 0 && IsObjectDescendantOf(m_objects, objectIndex, newParentIndex))
    {
        return false;
    }

    return true;
}

bool Scene::ReparentObject(int objectIndex, int newParentIndex)
{
    if (!CanReparentObject(objectIndex, newParentIndex))
    {
        return false;
    }

    SceneObject& object = m_objects[static_cast<std::size_t>(objectIndex)];
    if (object.GetParentIndex() == newParentIndex)
    {
        return true;
    }

    const glm::mat4 worldMatrix = GetWorldMatrix(objectIndex);

    glm::mat4 newParentWorldMatrix(1.0f);
    if (newParentIndex >= 0)
    {
        newParentWorldMatrix = GetWorldMatrix(newParentIndex);
    }

    object.SetParentIndex(newParentIndex);
    object.GetTransform().SetFromMatrix(glm::inverse(newParentWorldMatrix) * worldMatrix);
    MarkDirty();
    return true;
}

int Scene::AllocateSiblingOrder(int parentIndex) const
{
    int maxOrder = -1;
    for (const SceneObject& object : m_objects)
    {
        if (object.GetParentIndex() == parentIndex)
        {
            maxOrder = std::max(maxOrder, object.GetSiblingOrder());
        }
    }

    return maxOrder + 1;
}

void Scene::SetSiblingIndexAmongParent(int objectIndex, int parentIndex, int siblingIndex)
{
    std::vector<int> siblings = GetObjectChildren(m_objects, parentIndex);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    if (siblingIndex < 0)
    {
        siblingIndex = 0;
    }
    else if (siblingIndex > static_cast<int>(siblings.size()))
    {
        siblingIndex = static_cast<int>(siblings.size());
    }

    siblings.insert(siblings.begin() + siblingIndex, objectIndex);

    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        m_objects[static_cast<std::size_t>(siblings[static_cast<std::size_t>(index)])].SetSiblingOrder(index);
    }
}

bool Scene::CanPlaceObjectInHierarchy(
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode) const
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(m_objects.size()))
    {
        return false;
    }

    if (referenceIndex < 0 || referenceIndex >= static_cast<int>(m_objects.size()))
    {
        return false;
    }

    if (objectIndex == referenceIndex)
    {
        return false;
    }

    if (IsObjectDescendantOf(m_objects, objectIndex, referenceIndex))
    {
        return false;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        return CanReparentObject(objectIndex, referenceIndex);
    }

    const int referenceParent = m_objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    return CanReparentObject(objectIndex, referenceParent);
}

bool Scene::WouldPlaceObjectInHierarchyChange(
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode) const
{
    if (!CanPlaceObjectInHierarchy(objectIndex, referenceIndex, mode))
    {
        return false;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        if (m_objects[static_cast<std::size_t>(objectIndex)].GetParentIndex() != referenceIndex)
        {
            return true;
        }

        const std::vector<int> children = GetChildren(referenceIndex);
        return children.empty() || children.back() != objectIndex;
    }

    const int targetParent = m_objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    const int currentParent = m_objects[static_cast<std::size_t>(objectIndex)].GetParentIndex();

    std::vector<int> siblings = GetObjectChildren(m_objects, targetParent);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    int targetSiblingIndex = static_cast<int>(siblings.size());
    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        if (siblings[static_cast<std::size_t>(index)] == referenceIndex)
        {
            targetSiblingIndex = index + (mode == HierarchyInsertMode::After ? 1 : 0);
            break;
        }
    }

    if (currentParent != targetParent)
    {
        return true;
    }

    const std::vector<int> siblingsWithSelf = GetObjectChildren(m_objects, targetParent);
    const auto currentIterator = std::find(siblingsWithSelf.begin(), siblingsWithSelf.end(), objectIndex);
    if (currentIterator == siblingsWithSelf.end())
    {
        return true;
    }

    const int currentSiblingIndex = static_cast<int>(currentIterator - siblingsWithSelf.begin());
    return currentSiblingIndex != targetSiblingIndex;
}

bool Scene::PlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode)
{
    if (!CanPlaceObjectInHierarchy(objectIndex, referenceIndex, mode))
    {
        return false;
    }

    if (!WouldPlaceObjectInHierarchyChange(objectIndex, referenceIndex, mode))
    {
        return true;
    }

    if (mode == HierarchyInsertMode::AsChild)
    {
        if (!ReparentObject(objectIndex, referenceIndex))
        {
            return false;
        }

        const std::vector<int> children = GetChildren(referenceIndex);
        const int childIndex = static_cast<int>(
            std::find(children.begin(), children.end(), objectIndex) - children.begin());
        SetSiblingIndexAmongParent(objectIndex, referenceIndex, childIndex);
        MarkDirty();
        return true;
    }

    const int referenceParent = m_objects[static_cast<std::size_t>(referenceIndex)].GetParentIndex();
    if (!ReparentObject(objectIndex, referenceParent))
    {
        return false;
    }

    std::vector<int> siblings = GetObjectChildren(m_objects, referenceParent);
    siblings.erase(
        std::remove(siblings.begin(), siblings.end(), objectIndex),
        siblings.end());

    int insertIndex = 0;
    for (int index = 0; index < static_cast<int>(siblings.size()); ++index)
    {
        if (siblings[static_cast<std::size_t>(index)] == referenceIndex)
        {
            insertIndex = index + (mode == HierarchyInsertMode::After ? 1 : 0);
            break;
        }
    }

    SetSiblingIndexAmongParent(objectIndex, referenceParent, insertIndex);
    MarkDirty();
    return true;
}

bool Scene::PlaceObjectAtRootEnd(int objectIndex)
{
    if (!CanReparentObject(objectIndex, -1))
    {
        return false;
    }

    const std::vector<int> roots = GetRootObjectIndices();
    if (roots.empty())
    {
        if (!ReparentObject(objectIndex, -1))
        {
            return false;
        }

        m_objects[static_cast<std::size_t>(objectIndex)].SetSiblingOrder(0);
        return true;
    }

    return PlaceObjectInHierarchy(objectIndex, roots.back(), HierarchyInsertMode::After);
}

bool Scene::PlaceObjectAtRootBeginning(int objectIndex)
{
    if (!CanReparentObject(objectIndex, -1))
    {
        return false;
    }

    const std::vector<int> roots = GetRootObjectIndices();
    if (roots.empty())
    {
        if (!ReparentObject(objectIndex, -1))
        {
            return false;
        }

        m_objects[static_cast<std::size_t>(objectIndex)].SetSiblingOrder(0);
        return true;
    }

    return PlaceObjectInHierarchy(objectIndex, roots.front(), HierarchyInsertMode::Before);
}

void Scene::PruneUnusedImportedMeshes()
{
    std::unordered_set<Mesh*> referencedMeshes;
    referencedMeshes.reserve(m_objects.size());

    for (const SceneObject& object : m_objects)
    {
        if (object.HasMesh())
        {
            referencedMeshes.insert(object.GetMesh());
        }
    }

    m_importedMeshes.erase(
        std::remove_if(
            m_importedMeshes.begin(),
            m_importedMeshes.end(),
            [&](const std::unique_ptr<Mesh>& mesh) {
                return referencedMeshes.find(mesh.get()) == referencedMeshes.end();
            }),
        m_importedMeshes.end());
}

void Scene::MarkDirty()
{
    if (m_dirtyCallback)
    {
        m_dirtyCallback();
    }
}

void Scene::SetDirtyCallback(std::function<void()> callback)
{
    m_dirtyCallback = std::move(callback);
}

void Scene::ResetToDefault()
{
    m_objects.clear();
    m_importedMeshes.clear();
    m_selectedObjectIndex = -1;
    m_showLightGizmos = true;
    m_showGrid = true;
    m_nextDirectionalLightNumber = 2;
    m_nextPointLightNumber = 1;
    m_nextSpotLightNumber = 1;
    m_nextCubeNumber = 2;
    m_nextSphereNumber = 1;
    m_nextCylinderNumber = 1;
    m_nextCapsuleNumber = 1;
    m_nextPlaneNumber = 1;
    m_nextEmptyNumber = 1;
    m_nextImportNumber = 1;
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    SetupDefaultSunLight();
    SetupObjects();
}

const SceneLighting& Scene::GetLighting() const
{
    return m_lighting;
}

SceneLighting& Scene::GetLighting()
{
    return m_lighting;
}

IBL& Scene::GetIBL()
{
    return *m_ibl;
}

const IBL& Scene::GetIBL() const
{
    return *m_ibl;
}

const std::vector<SceneObject>& Scene::GetObjects() const
{
    return m_objects;
}

std::vector<SceneObject>& Scene::GetObjects()
{
    return m_objects;
}

SceneObject& Scene::GetObject(std::size_t index)
{
    return m_objects.at(index);
}

const SceneObject& Scene::GetObject(std::size_t index) const
{
    return m_objects.at(index);
}

int Scene::GetSelectedObjectIndex() const
{
    return m_selectedObjectIndex;
}

void Scene::SetSelectedObjectIndex(int selectedObjectIndex)
{
    if (m_objects.empty())
    {
        m_selectedObjectIndex = -1;
        return;
    }

    if (selectedObjectIndex < 0)
    {
        m_selectedObjectIndex = -1;
        return;
    }

    if (static_cast<std::size_t>(selectedObjectIndex) >= m_objects.size())
    {
        m_selectedObjectIndex = static_cast<int>(m_objects.size()) - 1;
        return;
    }

    m_selectedObjectIndex = selectedObjectIndex;
}

void Scene::ClearSelection()
{
    m_selectedObjectIndex = -1;
}

bool Scene::HasSelection() const
{
    return m_selectedObjectIndex >= 0 && static_cast<std::size_t>(m_selectedObjectIndex) < m_objects.size();
}

SceneEditor& Scene::GetSceneEditor()
{
    return *m_sceneEditor;
}

const SceneEditor& Scene::GetSceneEditor() const
{
    return *m_sceneEditor;
}

bool Scene::GetShowLightGizmos() const
{
    return m_showLightGizmos;
}

void Scene::SetShowLightGizmos(bool showLightGizmos)
{
    if (m_showLightGizmos != showLightGizmos)
    {
        m_showLightGizmos = showLightGizmos;
        MarkDirty();
    }
}

bool Scene::GetShowGrid() const
{
    return m_showGrid;
}

void Scene::SetShowGrid(bool showGrid)
{
    if (m_showGrid != showGrid)
    {
        m_showGrid = showGrid;
        MarkDirty();
    }
}

ScreenSpaceEffects& Scene::GetScreenSpaceEffects()
{
    return *m_screenSpaceEffects;
}

const ScreenSpaceEffects& Scene::GetScreenSpaceEffects() const
{
    return *m_screenSpaceEffects;
}

glm::vec3 Scene::GetSunDirection() const
{
    const int shadowLightIndex = m_lighting.GetShadowLightIndex();
    if (shadowLightIndex >= 0 &&
        static_cast<std::size_t>(shadowLightIndex) < m_lighting.GetLightCount())
    {
        return m_lighting.GetLight(static_cast<std::size_t>(shadowLightIndex)).GetDirection();
    }

    for (std::size_t lightIndex = 0; lightIndex < m_lighting.GetLightCount(); ++lightIndex)
    {
        const Light& light = m_lighting.GetLight(lightIndex);
        if (light.GetType() == LightType::Directional)
        {
            return light.GetDirection();
        }
    }

    return glm::vec3(0.0f, 1.0f, 0.0f);
}

void Scene::Update(
    Input& input,
    const Camera& camera,
    int framebufferWidth,
    int framebufferHeight,
    int windowWidth,
    int windowHeight,
    bool allowMouseInput,
    bool allowKeyboardInput)
{
    m_sceneEditor->Update(
        *this,
        camera,
        input,
        framebufferWidth,
        framebufferHeight,
        windowWidth,
        windowHeight,
        allowMouseInput,
        allowKeyboardInput);
}

void Scene::RenderShadowPass() const
{
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());

    for (std::size_t objectIndex = 0; objectIndex < m_objects.size(); ++objectIndex)
    {
        const SceneObject& object = m_objects[objectIndex];
        if (!object.CastsShadow() && !object.ReceivesShadow())
        {
            continue;
        }

        glm::vec3 objectBoundsMin;
        glm::vec3 objectBoundsMax;
        GetWorldBounds(static_cast<int>(objectIndex), objectBoundsMin, objectBoundsMax);
        boundsMin = glm::min(boundsMin, objectBoundsMin);
        boundsMax = glm::max(boundsMax, objectBoundsMax);
    }

    m_shadowMap->BeginPass(GetSunDirection(), boundsMin, boundsMax);

    m_shadowDepthShader->Use();
    m_shadowDepthShader->SetMat4("uLightSpaceMatrix", m_shadowMap->GetLightSpaceMatrix());

    for (std::size_t objectIndex = 0; objectIndex < m_objects.size(); ++objectIndex)
    {
        const SceneObject& object = m_objects[objectIndex];
        if (!object.IsRenderable() || !object.CastsShadow())
        {
            continue;
        }

        const GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
        if (object.GetMaterial().IsDoubleSided())
        {
            glDisable(GL_CULL_FACE);
        }

        m_shadowDepthShader->SetMat4("uModel", GetWorldMatrix(static_cast<int>(objectIndex)));
        object.GetMesh()->Draw();

        if (object.GetMaterial().IsDoubleSided() && cullFaceEnabled)
        {
            glEnable(GL_CULL_FACE);
        }
    }
}

void Scene::Render(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) const
{
    SyncLighting();

    RenderShadowPass();
    m_shadowMap->EndPass();

    const bool usePostProcess = m_screenSpaceEffects->IsEnabled();

    if (usePostProcess)
    {
        m_screenSpaceEffects->Resize(viewportWidth, viewportHeight);
        m_screenSpaceEffects->BeginScenePass();
    }

    glViewport(0, 0, viewportWidth, viewportHeight);

    for (std::size_t objectIndex = 0; objectIndex < m_objects.size(); ++objectIndex)
    {
        const SceneObject& object = m_objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        const glm::mat4 modelMatrix = GetWorldMatrix(static_cast<int>(objectIndex));
        const GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
        if (object.GetMaterial().IsDoubleSided())
        {
            glDisable(GL_CULL_FACE);
        }

        object.GetMaterial().Apply(
            camera,
            m_lighting,
            *m_ibl,
            modelMatrix,
            m_shadowMap.get(),
            object.ReceivesShadow(),
            usePostProcess);
        object.GetMesh()->Draw();

        if (object.GetMaterial().IsDoubleSided() && cullFaceEnabled)
        {
            glEnable(GL_CULL_FACE);
        }
    }

    if (usePostProcess)
    {
        if (m_showGrid)
        {
            m_grid->Draw(camera, true);
        }

        m_screenSpaceEffects->EndScenePass();
        m_screenSpaceEffects->Apply(camera, GetSunDirection(), viewportWidth, viewportHeight);
        m_screenSpaceEffects->BlitDepthToDefaultFramebuffer(viewportWidth, viewportHeight);
        m_sceneEditor->RenderSelectionOverlay(*this, camera);
    }
    else if (m_showGrid)
    {
        m_grid->Draw(camera, false);
    }

    if (m_showLightGizmos)
    {
        m_lightGizmos->Draw(
            camera,
            m_objects,
            [this](int objectIndex) { return GetWorldMatrix(objectIndex); },
            m_selectedObjectIndex);
    }

    if (!usePostProcess)
    {
        m_sceneEditor->RenderSelectionOverlay(*this, camera);
    }
}
