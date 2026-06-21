#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "engine/Constants.h"
#include "engine/SceneLighting.h"
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

class DemoScene
{
public:
    static constexpr float FloorHalfExtent = 12.0f;

    DemoScene();
    ~DemoScene();

    void Update(
        double deltaTime,
        bool paused,
        Input& input,
        bool allowObjectMovement,
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

    double GetAnimationTime() const;
    SceneEditor& GetSceneEditor();
    const SceneEditor& GetSceneEditor() const;

    void AddCubeObject();
    bool RemoveObject(std::size_t index);

    bool GetShowLightGizmos() const;
    void SetShowLightGizmos(bool showLightGizmos);
    int GetSelectedLightIndex() const;
    void SetSelectedLightIndex(int selectedLightIndex);

private:
    void SetupLighting();
    void SetupObjects();
    void HandleSelectedObjectMovement(Input& input, double deltaTime);
    glm::vec3 GetSunDirection() const;
    void RenderShadowPass() const;

    std::unique_ptr<Mesh> m_cubeMesh;
    std::unique_ptr<Mesh> m_floorMesh;
    std::vector<SceneObject> m_objects;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<LightGizmoRenderer> m_lightGizmos;
    std::unique_ptr<SceneEditor> m_sceneEditor;
    std::unique_ptr<ShadowMap> m_shadowMap;
    std::unique_ptr<IBL> m_ibl;
    std::unique_ptr<Shader> m_shadowDepthShader;
    SceneLighting m_lighting;
    double m_animationTime = 0.0;
    int m_selectedObjectIndex = 1;
    bool m_showLightGizmos = true;
    int m_selectedLightIndex = 0;
    int m_nextCubeNumber = 2;
};
