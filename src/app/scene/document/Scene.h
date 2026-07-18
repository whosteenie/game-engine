#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/lighting/Light.h"
#include "engine/scene/ScenePrimitive.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"

#include "app/scene/selection/SceneSelection.h"
#include "app/scene/rendering/RenderViewport.h"
#include "app/project/SceneImportedMeshPool.h"
#include "app/scene/spawn/SceneSpawnService.h"

class Camera;
class Mesh;
class SceneEditor;
class SceneImportService;
class SceneMeshLibrary;
class SceneObjectStore;
class SceneRenderer;
class SceneSelectionController;
class SceneSpawnService;
struct SceneSubtreeArchive;
struct ArchivedSelectionState;

enum class HierarchyInsertMode
{
    Before,
    After,
    AsChild
};

// Scene View-only presentation choice. This never changes the project's renderer tuning or the
// Game View output, which remains the runtime ground truth.
enum class SceneViewShadingMode
{
    FullRuntime,
    Lit,
    Unlit,
};

struct SceneRenderOptions
{
    bool showGrid = true;
    bool showCameraGizmos = true;
    bool showLightGizmos = true;
    bool showColliderGizmos = true;
    bool showEditorOverlay = true;
    bool enableShadowPass = true;
    SceneViewShadingMode shadingMode = SceneViewShadingMode::FullRuntime;
};

class Scene
{
public:
    static constexpr float FloorHalfExtent = 12.0f;

    Scene();
    ~Scene();

    SceneRenderer& GetRenderer();
    const SceneRenderer& GetRenderer() const;
    SceneObjectStore& GetObjectStore();
    const SceneObjectStore& GetObjectStore() const;
    SceneMeshLibrary& GetMeshLibrary();
    const SceneMeshLibrary& GetMeshLibrary() const;
    SceneSelectionController& GetSelectionController();
    const SceneSelectionController& GetSelectionController() const;
    SceneSpawnService& GetSpawnService();
    const SceneSpawnService& GetSpawnService() const;
    SceneImportService& GetImportService();
    const SceneImportService& GetImportService() const;

    void Render(
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        std::uintptr_t targetFramebuffer = 0,
        const SceneRenderOptions& options = SceneRenderOptions{},
        RenderViewport renderViewport = RenderViewport::SceneView) const;

    const std::vector<SceneObject>& GetObjects() const;
    std::vector<SceneObject>& GetObjects();
    void InvalidateAllMaterialCachedShaders();
    SceneObject& GetSceneObject(std::size_t index);
    const SceneObject& GetSceneObject(std::size_t index) const;
    int FindObjectIndex(SceneObjectId id) const;

    std::vector<SceneObjectId> GetSelectionIds() const;
    void SetSelectionByIds(const std::vector<SceneObjectId>& ids, SceneObjectId primary);
    const SceneSelection& GetSelection() const;
    int GetPrimarySelection() const;
    bool IsSelected(int objectIndex) const;
    void SetSelection(const std::vector<int>& indices, int primary);
    void SelectSingle(int objectIndex);
    void ToggleSelected(int objectIndex);
    void AddToSelection(const std::vector<int>& indices);
    void ClearSelection();
    bool HasSelection() const;
    bool TryGetViewFocusPoint(glm::vec3& center, float& radius) const;

    void BindSceneEditor(SceneEditor& editor);
    SceneEditor& GetSceneEditor();
    const SceneEditor& GetSceneEditor() const;
    SceneEditor* TryGetSceneEditor();
    const SceneEditor* TryGetSceneEditor() const;

    int AddObject(ScenePrimitive primitive, int parentIndex = -1);
    int AddEmptyObject(int parentIndex = -1);
    int AddLightObject(LightType type, int parentIndex = -1);
    int AddCameraObject(int parentIndex = -1);
    void EnsureUniqueMainCamera(int objectIndex);
    std::vector<int> ImportModel(
        const std::string& path,
        int parentIndex = -1,
        const std::string& projectRoot = {},
        bool isProjectAsset = false);

    static std::unique_ptr<Scene> CloneForPlayMode(const Scene& source, bool shareGpuResources = true);

    // Runtime play-mode clone shares GPU meshes and SceneRenderer with the edit scene.
    void UseSharedPlayModeResources(const Scene& editScene);

    void MarkDirty();
    // Request a render-boundary temporal reset for a structural scene edit (add/remove/restore).
    // Do not use for normal transform motion: valid motion vectors handle that case.
    void NotifyRenderContentChanged();
    void SetDirtyCallback(std::function<void()> callback);
    void SetDirtyNotificationsSuppressed(bool suppressed);

    bool RemoveObject(std::size_t index);
    bool RemoveSelectedObjects();
    int DuplicateObject(int objectIndex);
    std::vector<int> DuplicateSelectedObjects();
    bool ReparentObject(int objectIndex, int newParentIndex);
    bool CanReparentObject(int objectIndex, int newParentIndex) const;
    bool PlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode);
    bool CanPlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode) const;
    bool WouldPlaceObjectInHierarchyChange(int objectIndex, int referenceIndex, HierarchyInsertMode mode) const;
    bool PlaceObjectAtRootEnd(int objectIndex);
    bool PlaceObjectAtRootBeginning(int objectIndex);

    glm::mat4 GetWorldMatrix(int objectIndex) const;
    void SetObjectWorldMatrix(int objectIndex, const glm::mat4& worldMatrix);
    glm::mat4 GetGizmoWorldMatrix(int objectIndex) const;
    glm::mat4 GetSelectionGizmoWorldMatrix(bool worldSpace) const;
    void GetLocalSelectionBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const;
    void ApplyGizmoWorldMatrix(
        int objectIndex,
        const glm::mat4& oldGizmoWorldMatrix,
        const glm::mat4& newGizmoWorldMatrix);
    void ApplySelectionGizmoWorldMatrix(
        const glm::mat4& oldGizmoWorldMatrix,
        const glm::mat4& newGizmoWorldMatrix);
    void GetWorldBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const;
    std::vector<int> GetChildren(int objectIndex) const;
    std::vector<int> GetRootObjectIndices() const;

    bool GetShowLightGizmos() const;
    void SetShowLightGizmos(bool showLightGizmos);
    bool GetShowGrid() const;
    void SetShowGrid(bool showGrid);

    void ResetToDefault();
    // Prepare for a project boundary. Warm renderer pipelines and current CPU assets are retained
    // by default so deserialization can reuse matching meshes; pass false for a completely fresh
    // scene and renderer.
    void ResetForProjectTransition(bool preserveRendererResources = true);
    void ClearImportedModelCache();

    bool CreateDeleteArchive(
        const std::vector<int>& rootIndices,
        SceneSubtreeArchive& archive,
        bool transferImportedMeshOwnership = true);
    bool DeleteUsingArchive(const SceneSubtreeArchive& archive);
    bool RestoreDeleteArchive(SceneSubtreeArchive& archive, const ArchivedSelectionState& selection);
    std::vector<int> InsertSubtreeArchive(
        SceneSubtreeArchive& archive,
        int referenceIndex,
        HierarchyInsertMode rootPlacement);

private:
    void CollectDescendantIndices(int objectIndex, std::vector<int>& outIndices) const;
    std::string MakeDuplicateObjectName(const std::string& sourceName) const;

    std::unique_ptr<SceneMeshLibrary> m_meshLibrary;
    std::unique_ptr<SceneObjectStore> m_objectStore;
    std::unique_ptr<SceneSpawnService> m_spawnService;
    std::unique_ptr<SceneImportService> m_importService;
    SceneEditor* m_sceneEditor = nullptr;
    std::unique_ptr<SceneRenderer> m_renderer;
    SceneMeshLibrary* m_sharedMeshLibrary = nullptr;
    SceneRenderer* m_sharedRenderer = nullptr;
    std::unique_ptr<SceneSelectionController> m_selectionController;
    bool m_showLightGizmos = true;
    bool m_showGrid = true;
    bool m_dirtyNotificationsSuppressed = false;
    std::function<void()> m_dirtyCallback;
};
