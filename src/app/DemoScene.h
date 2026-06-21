#pragma once

#include <glm/glm.hpp>
#include <memory>

#include "engine/SceneLighting.h"

class Camera;
class Input;
class Material;
class Mesh;
class GridRenderer;

// Sandbox scene for engine development. Replace or complement with src/game/ later.
class DemoScene
{
public:
    DemoScene();
    ~DemoScene();

    void Update(double deltaTime, bool paused, Input& input);
    void Render(const Camera& camera, const Material& material) const;

    const SceneLighting& GetLighting() const;

private:
    void HandleMovement(Input& input, double deltaTime);
    void SetupLighting();
    glm::mat4 BuildModelMatrix() const;

    std::unique_ptr<Mesh> m_mesh;
    std::unique_ptr<GridRenderer> m_grid;
    SceneLighting m_lighting;
    glm::vec3 m_position = glm::vec3(0.0f);
    double m_animationTime = 0.0;
};
