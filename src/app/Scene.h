#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "engine/Constants.h"
#include "engine/SceneLighting.h"
#include "engine/ScenePrimitive.h"
#include "engine/Shader.h"
#include "engine/ShadowMap.h"
#include "engine/IBL.h"
#include "engine/SceneObject.h"

class Camera;
class Input;
class Mesh;
class GridRenderer;
class LightGizmoRenderer;
class SceneEditor;
class ScreenSpaceEffects;

enum class HierarchyInsertMode
{
    Before,
    After,
    AsChild
};

class Scene
{
public:
    static constexpr float FloorHalfExtent = 12.0f;

    Scene();
    ~Scene();

    void Update(
        Input& input,
        const Camera& camera,
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight,
        bool allowMouseInput,
        bool allowKeyboardInput);

    void Render(const Camera& camera, int viewportWidth, int viewportHeight) const;

    const SceneLighting& GetLighting() const;
    SceneLighting& GetLighting();
    IBL& GetIBL();

    const std::vector<SceneObject>& GetObjects() const;
    std::vector<SceneObject>& GetObjects();
    SceneObject& GetObject(std::size_t index);
    const SceneObject& GetObject(std::size_t index) const;

    int GetSelectedObjectIndex() const;
    void SetSelectedObjectIndex(int selectedObjectIndex);
    void ClearSelection();
    bool HasSelection() const;

    SceneEditor& GetSceneEditor();
    const SceneEditor& GetSceneEditor() const;

    int AddObject(ScenePrimitive primitive, int parentIndex = -1);
    int AddEmptyObject(int parentIndex = -1);
    std::vector<int> ImportModel(const std::string& path, int parentIndex = -1);
    const std::string& GetLastImportError() const;
    const std::string& GetLastImportWarning() const;
    bool RemoveObject(std::size_t index);
    int DuplicateObject(int objectIndex);
    bool ReparentObject(int objectIndex, int newParentIndex);
    bool CanReparentObject(int objectIndex, int newParentIndex) const;
    bool PlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode);
    bool CanPlaceObjectInHierarchy(int objectIndex, int referenceIndex, HierarchyInsertMode mode) const;
    bool WouldPlaceObjectInHierarchyChange(int objectIndex, int referenceIndex, HierarchyInsertMode mode) const;
    bool PlaceObjectAtRootEnd(int objectIndex);
    bool PlaceObjectAtRootBeginning(int objectIndex);

    glm::mat4 GetWorldMatrix(int objectIndex) const;
    glm::mat4 GetGizmoWorldMatrix(int objectIndex) const;
    void GetLocalSelectionBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const;
    void ApplyGizmoWorldMatrix(int objectIndex, const glm::mat4& gizmoWorldMatrix);
    void GetWorldBounds(int objectIndex, glm::vec3& boundsMin, glm::vec3& boundsMax) const;
    std::vector<int> GetChildren(int objectIndex) const;
    std::vector<int> GetRootObjectIndices() const;

    bool GetShowLightGizmos() const;
    void SetShowLightGizmos(bool showLightGizmos);
    int GetSelectedLightIndex() const;
    void SetSelectedLightIndex(int selectedLightIndex);
    void ClearLightSelection();
    bool HasLightSelection() const;

    ScreenSpaceEffects& GetScreenSpaceEffects();
    const ScreenSpaceEffects& GetScreenSpaceEffects() const;

private:
    void SetupLighting();
    void SetupObjects();
    glm::vec3 GetSunDirection() const;
    void RenderShadowPass() const;

    Mesh* GetMeshForPrimitive(ScenePrimitive primitive);
    int GetNextObjectNumber(ScenePrimitive primitive);
    void RemapParentIndicesAfterRemoval(int removedIndex);
    void CollectDescendantIndices(int objectIndex, std::vector<int>& outIndices) const;
    void PruneUnusedImportedMeshes();
    int AllocateSiblingOrder(int parentIndex) const;
    void SetSiblingIndexAmongParent(int objectIndex, int parentIndex, int siblingIndex);
    std::string MakeDuplicateObjectName(const std::string& sourceName) const;

    std::unique_ptr<Mesh> m_cubeMesh;
    std::unique_ptr<Mesh> m_sphereMesh;
    std::unique_ptr<Mesh> m_cylinderMesh;
    std::unique_ptr<Mesh> m_capsuleMesh;
    std::unique_ptr<Mesh> m_planeMesh;
    std::vector<std::unique_ptr<Mesh>> m_importedMeshes;
    std::vector<SceneObject> m_objects;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<LightGizmoRenderer> m_lightGizmos;
    std::unique_ptr<SceneEditor> m_sceneEditor;
    std::unique_ptr<ShadowMap> m_shadowMap;
    std::unique_ptr<IBL> m_ibl;
    std::unique_ptr<ScreenSpaceEffects> m_screenSpaceEffects;
    std::unique_ptr<Shader> m_shadowDepthShader;
    SceneLighting m_lighting;
    int m_selectedObjectIndex = 1;
    bool m_showLightGizmos = true;
    int m_selectedLightIndex = 0;
    int m_nextCubeNumber = 2;
    int m_nextSphereNumber = 1;
    int m_nextCylinderNumber = 1;
    int m_nextCapsuleNumber = 1;
    int m_nextPlaneNumber = 1;
    int m_nextEmptyNumber = 1;
    int m_nextImportNumber = 1;
    std::string m_lastImportError;
    std::string m_lastImportWarning;
};
