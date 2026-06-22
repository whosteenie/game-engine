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
#include "engine/SceneHierarchy.h"
#include "primitives/Cube.h"
#include "primitives/Sphere.h"
#include "primitives/Cylinder.h"
#include "primitives/Capsule.h"
#include "primitives/Plane.h"
#include "engine/GridRenderer.h"
#include "engine/LightGizmoRenderer.h"
#include "engine/SceneLighting.h"
#include "engine/ShadowMap.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <limits>
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
      m_shadowDepthShader(std::make_unique<Shader>(
          EngineConstants::ShadowDepthVertexShader,
          EngineConstants::ShadowDepthFragmentShader))
{
    SetupLighting();
    SetupObjects();
}

Scene::~Scene() = default;

void Scene::SetupLighting()
{
    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.45f, 0.7f, 0.55f));
    const glm::vec3 sunColor(1.0f, 0.97f, 0.92f);

    m_lighting.AddLight(Light::MakeDirectional(
        sunDirection,
        sunColor,
        2.5f));

    m_lighting.SetShadowLightIndex(0);
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
        true);

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
        true);

    m_selectedObjectIndex = 1;
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

Mesh* Scene::GetMeshForPrimitive(ScenePrimitive primitive)
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
        parentIndex);

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
        parentIndex);

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

std::vector<int> Scene::ImportModel(const std::string& path, int parentIndex)
{
    m_lastImportError.clear();
    m_lastImportWarning.clear();

    ImportedModel importedModel = LoadModelFromFile(path);
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
            parentSceneIndex);

        importedSceneIndices.push_back(static_cast<int>(m_objects.size()) - 1);
    }

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

    return true;
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
    m_showLightGizmos = showLightGizmos;
}

int Scene::GetSelectedLightIndex() const
{
    return m_selectedLightIndex;
}

void Scene::SetSelectedLightIndex(int selectedLightIndex)
{
    if (m_lighting.GetLightCount() == 0)
    {
        m_selectedLightIndex = -1;
        return;
    }

    if (selectedLightIndex < 0)
    {
        m_selectedLightIndex = -1;
        return;
    }

    if (static_cast<std::size_t>(selectedLightIndex) >= m_lighting.GetLightCount())
    {
        m_selectedLightIndex = static_cast<int>(m_lighting.GetLightCount()) - 1;
        return;
    }

    m_selectedLightIndex = selectedLightIndex;
}

void Scene::ClearLightSelection()
{
    m_selectedLightIndex = -1;
}

bool Scene::HasLightSelection() const
{
    return m_selectedLightIndex >= 0 &&
        static_cast<std::size_t>(m_selectedLightIndex) < m_lighting.GetLightCount();
}

glm::vec3 Scene::GetSunDirection() const
{
    const auto& lights = m_lighting.GetLights();
    if (lights.empty())
    {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }

    return lights.front().GetDirection();
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
    RenderShadowPass();
    m_shadowMap->EndPass();

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
            object.ReceivesShadow());
        object.GetMesh()->Draw();

        if (object.GetMaterial().IsDoubleSided() && cullFaceEnabled)
        {
            glEnable(GL_CULL_FACE);
        }
    }

    m_grid->Draw(camera);

    if (m_showLightGizmos)
    {
        m_lightGizmos->Draw(camera, m_lighting, m_selectedLightIndex);
    }

    m_sceneEditor->RenderSelectionOverlay(*this, camera);
}
