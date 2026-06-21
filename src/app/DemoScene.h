#pragma once

#include <glm/glm.hpp>
#include <memory>

#include "engine/Constants.h"
#include "engine/Material.h"
#include "engine/SceneLighting.h"
#include "engine/Shader.h"
#include "engine/ShadowMap.h"
#include "engine/IBL.h"

class Camera;
class Input;
class Mesh;
class GridRenderer;

// Sandbox scene for engine development. Replace or complement with src/game/ later.
class DemoScene
{
public:
    static constexpr float FloorHalfExtent = 12.0f;

    DemoScene();
    ~DemoScene();

    void Update(double deltaTime, bool paused, Input& input, bool allowCubeMovement);
    void Render(const Camera& camera, const Material& cubeMaterial, int viewportWidth, int viewportHeight) const;

    const SceneLighting& GetLighting() const;
    SceneLighting& GetLighting();
    IBL& GetIBL();
    Material& GetFloorMaterial();

private:
    void HandleMovement(Input& input, double deltaTime);
    void SetupLighting();
    glm::mat4 BuildCubeModelMatrix() const;
    glm::mat4 BuildFloorModelMatrix() const;
    glm::vec3 GetSunDirection() const;
    void RenderShadowPass(const glm::mat4& cubeModel) const;

    std::unique_ptr<Mesh> m_cubeMesh;
    std::unique_ptr<Mesh> m_floorMesh;
    std::unique_ptr<GridRenderer> m_grid;
    std::unique_ptr<ShadowMap> m_shadowMap;
    std::unique_ptr<IBL> m_ibl;
    std::unique_ptr<Shader> m_shadowDepthShader;
    std::unique_ptr<Material> m_floorMaterial;
    SceneLighting m_lighting;
    glm::vec3 m_position = glm::vec3(0.0f, 1.5f, 0.0f);
    double m_animationTime = 0.0;
};
