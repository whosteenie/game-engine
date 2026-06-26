#include "app/scene/Scene.h"
#include "app/scene/SceneHierarchyOps.h"
#include "app/scene/SceneImportService.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectOperations.h"
#include "app/scene/SceneObjectStore.h"
#include "app/scene/SceneRenderer.h"
#include "app/scene/SceneSelectionController.h"
#include "app/scene/SceneSpawnService.h"

#include "app/scene/SceneEditor.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/lighting/Light.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/scene/SceneObjectComponents.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <limits>

namespace
{
    void CopyRendererSettings(const Scene& source, Scene& destination)
    {
        const SceneRenderer& sourceRenderer = source.GetRenderer();
        SceneRenderer& destinationRenderer = destination.GetRenderer();

        destinationRenderer.GetIBL().SetEnvironmentIntensity(
            sourceRenderer.GetIBL().GetEnvironmentIntensity());

        EnvironmentMap& destinationEnvironment = destinationRenderer.GetEnvironmentMap();
        const EnvironmentMap& sourceEnvironment = sourceRenderer.GetEnvironmentMap();
        destinationEnvironment.SetEnabled(sourceEnvironment.IsEnabled());
        destinationEnvironment.SetBackgroundMode(sourceEnvironment.GetBackgroundMode());
        destinationEnvironment.SetHdrPath(sourceEnvironment.GetHdrPath());
        destinationEnvironment.SetRotationDegrees(sourceEnvironment.GetRotationDegrees());
        destinationEnvironment.SetExposure(sourceEnvironment.GetExposure());
        destinationEnvironment.SetSolidBackgroundColorSrgb(
            sourceEnvironment.GetSolidBackgroundColorSrgb());

        const ScreenSpaceEffects& sourceEffects = sourceRenderer.GetScreenSpaceEffects();
        ScreenSpaceEffects& destinationEffects = destinationRenderer.GetScreenSpaceEffects();
        destinationEffects.SetEnabled(sourceEffects.IsEnabled());
        destinationEffects.SetSsaoEnabled(sourceEffects.IsSsaoEnabled());
        destinationEffects.SetSsaoRadius(sourceEffects.GetSsaoRadius());
        destinationEffects.SetSsaoBias(sourceEffects.GetSsaoBias());
        destinationEffects.SetSsaoPower(sourceEffects.GetSsaoPower());
        destinationEffects.SetAoStrength(sourceEffects.GetAoStrength());
        destinationEffects.SetExposure(sourceEffects.GetExposure());
        destinationEffects.SetTonemapMode(sourceEffects.GetTonemapMode());
        destinationEffects.SetBloomEnabled(sourceEffects.IsBloomEnabled());
        destinationEffects.SetBloomThreshold(sourceEffects.GetBloomThreshold());
        destinationEffects.SetBloomSoftKnee(sourceEffects.GetBloomSoftKnee());
        destinationEffects.SetBloomIntensity(sourceEffects.GetBloomIntensity());
        destinationEffects.SetBloomBlurRadius(sourceEffects.GetBloomBlurRadius());
        destinationEffects.SetAntiAliasingMode(sourceEffects.GetAntiAliasingMode());
        destinationEffects.SetFxaaSubpixQuality(sourceEffects.GetFxaaSubpixQuality());
        destinationEffects.SetFxaaEdgeThreshold(sourceEffects.GetFxaaEdgeThreshold());
        destinationEffects.SetRenderScale(sourceEffects.GetRenderScale());
        destinationEffects.SetTaaBlendFactor(sourceEffects.GetTaaBlendFactor());
        destinationEffects.SetSmaaThreshold(sourceEffects.GetSmaaThreshold());
        destinationEffects.SetSmaaSearchSteps(sourceEffects.GetSmaaSearchSteps());
        destinationEffects.SetSsaoBlurDepthThreshold(sourceEffects.GetSsaoBlurDepthThreshold());

        destinationRenderer.SetTextureFilterMode(sourceRenderer.GetTextureFilterMode());
        destinationRenderer.SetTextureAnisotropy(sourceRenderer.GetTextureAnisotropy());
        destinationRenderer.SetTextureMipBias(sourceRenderer.GetTextureMipBias());
        destinationRenderer.GetDirectionalShadowSettings() =
            sourceRenderer.GetDirectionalShadowSettings();
    }
}

Scene::Scene()
    : m_meshLibrary(std::make_unique<SceneMeshLibrary>(FloorHalfExtent)),
      m_objectStore(std::make_unique<SceneObjectStore>()),
      m_spawnService(std::make_unique<SceneSpawnService>()),
      m_importService(std::make_unique<SceneImportService>()),
      m_renderer(std::make_unique<SceneRenderer>()),
      m_selectionController(std::make_unique<SceneSelectionController>())
{
    m_spawnService->SetupDefaultSunLight(*this);
}

Scene::~Scene() = default;

SceneRenderer& Scene::GetRenderer()
{
    return *m_renderer;
}

const SceneRenderer& Scene::GetRenderer() const
{
    return *m_renderer;
}

SceneObjectStore& Scene::GetObjectStore()
{
    return *m_objectStore;
}

const SceneObjectStore& Scene::GetObjectStore() const
{
    return *m_objectStore;
}

SceneMeshLibrary& Scene::GetMeshLibrary()
{
    return *m_meshLibrary;
}

const SceneMeshLibrary& Scene::GetMeshLibrary() const
{
    return *m_meshLibrary;
}

SceneSelectionController& Scene::GetSelectionController()
{
    return *m_selectionController;
}

const SceneSelectionController& Scene::GetSelectionController() const
{
    return *m_selectionController;
}

SceneSpawnService& Scene::GetSpawnService()
{
    return *m_spawnService;
}

const SceneSpawnService& Scene::GetSpawnService() const
{
    return *m_spawnService;
}

SceneImportService& Scene::GetImportService()
{
    return *m_importService;
}

const SceneImportService& Scene::GetImportService() const
{
    return *m_importService;
}

std::unique_ptr<Scene> Scene::CloneForPlayMode(const Scene& source)
{
    auto clone = std::make_unique<Scene>();
    clone->m_objectStore->Objects().clear();
    clone->m_objectStore->Objects().reserve(source.m_objectStore->Objects().size());

    const auto remapMesh = [&source](Mesh* sourceMesh, Scene& destination) -> Mesh* {
        if (sourceMesh == nullptr)
        {
            return nullptr;
        }

        const ScenePrimitive primitives[] = {
            ScenePrimitive::Cube,
            ScenePrimitive::Sphere,
            ScenePrimitive::Cylinder,
            ScenePrimitive::Capsule,
            ScenePrimitive::Plane,
        };

        for (ScenePrimitive primitive : primitives)
        {
            if (sourceMesh == source.GetMeshLibrary().GetPrimitive(primitive))
            {
                return destination.GetMeshLibrary().GetPrimitive(primitive);
            }
        }

        if (source.GetMeshLibrary().IsImportedMesh(sourceMesh))
        {
            return destination.GetMeshLibrary().AdoptClonedImportedMesh(*sourceMesh);
        }

        return nullptr;
    };

    for (const SceneObject& sourceObject : source.m_objectStore->Objects())
    {
        SceneObjectComponentSnapshot components = CaptureSceneObjectComponents(sourceObject);

        clone->m_objectStore->Objects().emplace_back(
            sourceObject.GetName(),
            remapMesh(sourceObject.GetMesh(), *clone),
            std::move(components.material),
            sourceObject.GetLocalBoundsMin(),
            sourceObject.GetLocalBoundsMax(),
            sourceObject.GetTransform(),
            sourceObject.CastsShadow(),
            sourceObject.ReceivesShadow(),
            sourceObject.GetParentIndex(),
            sourceObject.GetSiblingOrder(),
            std::move(components.light),
            std::move(components.camera),
            std::move(components.rigidBody),
            std::move(components.collider),
            sourceObject.GetId());

        SceneObject& clonedObject = clone->m_objectStore->Objects().back();
        if (!sourceObject.GetImportAssetPath().empty())
        {
            clonedObject.SetImportSource(
                sourceObject.GetImportAssetPath(),
                sourceObject.GetImportNodeIndex());
        }

        ApplySceneObjectInspectorOrder(clonedObject, components.inspectorComponentOrder);
    }

    clone->m_selectionController->SetState(source.GetSelection());
    clone->GetSpawnService().SetCounters(source.GetSpawnService().GetCounters());
    clone->GetObjectStore().SetNextId(source.GetObjectStore().GetNextId());
    clone->SetShowLightGizmos(source.GetShowLightGizmos());
    clone->SetShowGrid(source.GetShowGrid());
    clone->GetRenderer().GetLighting() = source.GetRenderer().GetLighting();
    CopyRendererSettings(source, *clone);

    return clone;
}

int Scene::AddObject(ScenePrimitive primitive, int parentIndex)
{
    return m_spawnService->AddObject(*this, primitive, parentIndex);
}

int Scene::AddEmptyObject(int parentIndex)
{
    return m_spawnService->AddEmptyObject(*this, parentIndex);
}

int Scene::AddLightObject(LightType type, int parentIndex)
{
    return m_spawnService->AddLightObject(*this, type, parentIndex);
}

int Scene::AddCameraObject(int parentIndex)
{
    return m_spawnService->AddCameraObject(*this, parentIndex);
}

void Scene::EnsureUniqueMainCamera(int objectIndex)
{
    m_spawnService->EnsureUniqueMainCamera(*this, objectIndex);
}

glm::mat4 Scene::GetWorldMatrix(int objectIndex) const
{
    return GetObjectWorldMatrix(m_objectStore->Objects(), objectIndex);
}

void Scene::SetObjectWorldMatrix(int objectIndex, const glm::mat4& worldMatrix)
{
    ::SetObjectWorldMatrix(m_objectStore->Objects(), objectIndex, worldMatrix);
    MarkDirty();
}

glm::mat4 Scene::GetGizmoWorldMatrix(int objectIndex) const
{
    return GetObjectGizmoWorldMatrix(m_objectStore->Objects(), objectIndex);
}

void Scene::GetLocalSelectionBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const
{
    GetObjectLocalSelectionBounds(m_objectStore->Objects(), objectIndex, boundsMin, boundsMax);
}

void Scene::ApplyGizmoWorldMatrix(
    int objectIndex,
    const glm::mat4& oldGizmoWorldMatrix,
    const glm::mat4& newGizmoWorldMatrix)
{
    ApplyObjectGizmoWorldMatrix(
        m_objectStore->Objects(),
        objectIndex,
        oldGizmoWorldMatrix,
        newGizmoWorldMatrix);
    MarkDirty();
}

glm::mat4 Scene::GetSelectionGizmoWorldMatrix(bool worldSpace) const
{
    const std::vector<int>& selectedIndices = m_selectionController->Get().indices;
    if (selectedIndices.empty())
    {
        return glm::mat4(1.0f);
    }

    if (selectedIndices.size() == 1)
    {
        return GetGizmoWorldMatrix(selectedIndices.front());
    }

    return GetGroupSelectionGizmoWorldMatrix(
        m_objectStore->Objects(),
        selectedIndices,
        m_selectionController->Get().primary,
        worldSpace);
}

void Scene::ApplySelectionGizmoWorldMatrix(
    const glm::mat4& oldGizmoWorldMatrix,
    const glm::mat4& newGizmoWorldMatrix)
{
    const std::vector<int>& selectedIndices = m_selectionController->Get().indices;
    if (selectedIndices.empty())
    {
        return;
    }

    if (selectedIndices.size() == 1)
    {
        ApplyGizmoWorldMatrix(
            selectedIndices.front(),
            oldGizmoWorldMatrix,
            newGizmoWorldMatrix);
        return;
    }

    ApplyGroupSelectionGizmoWorldMatrix(
        m_objectStore->Objects(),
        selectedIndices,
        oldGizmoWorldMatrix,
        newGizmoWorldMatrix);
    MarkDirty();
}

void Scene::GetWorldBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const
{
    GetObjectWorldBounds(m_objectStore->Objects(), objectIndex, boundsMin, boundsMax);
}

std::vector<int> Scene::GetChildren(int objectIndex) const
{
    return GetObjectChildren(m_objectStore->Objects(), objectIndex);
}

std::vector<int> Scene::GetRootObjectIndices() const
{
    return ::GetRootObjectIndices(m_objectStore->Objects());
}

void Scene::CollectDescendantIndices(int objectIndex, std::vector<int>& outIndices) const
{
    SceneHierarchyOps::CollectDescendantIndices(m_objectStore->Objects(), objectIndex, outIndices);
}

std::vector<int> Scene::ImportModel(const std::string& path, int parentIndex, const std::string& projectRoot)
{
    return m_importService->ImportModel(*this, path, parentIndex, projectRoot);
}

bool Scene::RemoveObject(std::size_t index)
{
    return SceneObjectOperations::RemoveObject(*this, index);
}

bool Scene::RemoveSelectedObjects()
{
    return SceneObjectOperations::RemoveSelectedObjects(*this);
}

std::string Scene::MakeDuplicateObjectName(const std::string& sourceName) const
{
    return SceneObjectOperations::MakeDuplicateObjectName(*this, sourceName);
}

int Scene::DuplicateObject(int objectIndex)
{
    return SceneObjectOperations::DuplicateObject(*this, objectIndex);
}

std::vector<int> Scene::DuplicateSelectedObjects()
{
    return SceneObjectOperations::DuplicateSelectedObjects(*this);
}

bool Scene::CanReparentObject(int objectIndex, int newParentIndex) const
{
    return SceneHierarchyOps::CanReparentObject(m_objectStore->Objects(), objectIndex, newParentIndex);
}

bool Scene::ReparentObject(int objectIndex, int newParentIndex)
{
    if (!CanReparentObject(objectIndex, newParentIndex))
    {
        return false;
    }

    const bool wouldChange =
        m_objectStore->Objects()[static_cast<std::size_t>(objectIndex)].GetParentIndex() != newParentIndex;
    const bool success = SceneHierarchyOps::ReparentObject(
        m_objectStore->Objects(),
        objectIndex,
        newParentIndex,
        [this](int index) { return GetWorldMatrix(index); });
    if (success && wouldChange)
    {
        MarkDirty();
    }
    return success;
}

bool Scene::CanPlaceObjectInHierarchy(
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode) const
{
    return SceneHierarchyOps::CanPlaceObjectInHierarchy(
        m_objectStore->Objects(),
        objectIndex,
        referenceIndex,
        mode);
}

bool Scene::WouldPlaceObjectInHierarchyChange(
    int objectIndex,
    int referenceIndex,
    HierarchyInsertMode mode) const
{
    return SceneHierarchyOps::WouldPlaceObjectInHierarchyChange(
        m_objectStore->Objects(),
        objectIndex,
        referenceIndex,
        mode);
}

bool Scene::PlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode)
{
    const bool wouldChange = WouldPlaceObjectInHierarchyChange(objectIndex, referenceIndex, mode);
    const bool success = SceneHierarchyOps::PlaceObjectInHierarchy(
        m_objectStore->Objects(),
        objectIndex,
        referenceIndex,
        mode,
        [this](int index) { return GetWorldMatrix(index); });
    if (success && wouldChange)
    {
        MarkDirty();
    }
    return success;
}

bool Scene::PlaceObjectAtRootEnd(int objectIndex)
{
    const bool changed = SceneHierarchyOps::PlaceObjectAtRootEnd(
        m_objectStore->Objects(),
        objectIndex,
        [this](int index) { return GetWorldMatrix(index); });
    if (changed)
    {
        MarkDirty();
    }
    return changed;
}

bool Scene::PlaceObjectAtRootBeginning(int objectIndex)
{
    const bool changed = SceneHierarchyOps::PlaceObjectAtRootBeginning(
        m_objectStore->Objects(),
        objectIndex,
        [this](int index) { return GetWorldMatrix(index); });
    if (changed)
    {
        MarkDirty();
    }
    return changed;
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
    m_spawnService->ResetToDefault(*this);
}

const std::vector<SceneObject>& Scene::GetObjects() const
{
    return m_objectStore->Objects();
}

std::vector<SceneObject>& Scene::GetObjects()
{
    return m_objectStore->Objects();
}

SceneObject& Scene::GetSceneObject(std::size_t index)
{
    return m_objectStore->At(index);
}

const SceneObject& Scene::GetSceneObject(std::size_t index) const
{
    return m_objectStore->At(index);
}

int Scene::FindObjectIndex(SceneObjectId id) const
{
    return m_objectStore->FindIndex(id);
}

std::vector<SceneObjectId> Scene::GetSelectionIds() const
{
    return m_selectionController->GetIds(m_objectStore->Objects());
}

void Scene::SetSelectionByIds(const std::vector<SceneObjectId>& ids, SceneObjectId primary)
{
    m_selectionController->SetByIds(*m_objectStore, ids, primary);
}

const SceneSelection& Scene::GetSelection() const
{
    return m_selectionController->Get();
}

int Scene::GetPrimarySelection() const
{
    return m_selectionController->Primary();
}

bool Scene::IsSelected(int objectIndex) const
{
    return m_selectionController->IsSelected(objectIndex);
}

void Scene::SetSelection(const std::vector<int>& indices, int primary)
{
    m_selectionController->Set(m_objectStore->Objects(), indices, primary);
}

void Scene::SelectSingle(int objectIndex)
{
    m_selectionController->SelectSingle(m_objectStore->Objects(), objectIndex);
}

void Scene::ToggleSelected(int objectIndex)
{
    m_selectionController->Toggle(m_objectStore->Objects(), objectIndex);
}

void Scene::AddToSelection(const std::vector<int>& indices)
{
    m_selectionController->Add(m_objectStore->Objects(), indices);
}

void Scene::ClearSelection()
{
    m_selectionController->Clear();
}

bool Scene::HasSelection() const
{
    return m_selectionController->HasSelection();
}

bool Scene::TryGetViewFocusPoint(glm::vec3& center, float& radius) const
{
    const std::vector<int>& selectedIndices = m_selectionController->Get().indices;
    if (selectedIndices.empty())
    {
        return false;
    }

    glm::vec3 boundsMin = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool hasBounds = false;

    for (int objectIndex : selectedIndices)
    {
        glm::vec3 objectBoundsMin;
        glm::vec3 objectBoundsMax;
        GetWorldBounds(objectIndex, objectBoundsMin, objectBoundsMax);

        if (!hasBounds)
        {
            boundsMin = objectBoundsMin;
            boundsMax = objectBoundsMax;
            hasBounds = true;
        }
        else
        {
            boundsMin = glm::min(boundsMin, objectBoundsMin);
            boundsMax = glm::max(boundsMax, objectBoundsMax);
        }
    }

    if (!hasBounds)
    {
        return false;
    }

    center = (boundsMin + boundsMax) * 0.5f;
    radius = glm::length((boundsMax - boundsMin) * 0.5f);
    if (radius < 0.05f)
    {
        radius = 0.5f;
    }

    return true;
}

void Scene::BindSceneEditor(SceneEditor& editor)
{
    m_sceneEditor = &editor;
}

SceneEditor& Scene::GetSceneEditor()
{
    if (m_sceneEditor == nullptr)
    {
        throw std::runtime_error("Scene editor is not bound");
    }

    return *m_sceneEditor;
}

const SceneEditor& Scene::GetSceneEditor() const
{
    if (m_sceneEditor == nullptr)
    {
        throw std::runtime_error("Scene editor is not bound");
    }

    return *m_sceneEditor;
}

SceneEditor* Scene::TryGetSceneEditor()
{
    return m_sceneEditor;
}

const SceneEditor* Scene::TryGetSceneEditor() const
{
    return m_sceneEditor;
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

void Scene::Render(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight,
    std::uintptr_t targetFramebuffer,
    const SceneRenderOptions& options) const
{
    m_renderer->Render(*this, camera, viewportWidth, viewportHeight, targetFramebuffer, options);
}
