#include "app/DemoScene.h"

#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/Mesh.h"
#include "primitives/Cube.h"
#include "engine/GridRenderer.h"
#include "engine/SceneLighting.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

DemoScene::DemoScene()
    : m_mesh(CreateCubeMesh()),
      m_grid(std::make_unique<GridRenderer>())
{
    SetupLighting();
}

DemoScene::~DemoScene() = default;

void DemoScene::SetupLighting()
{
    const glm::vec3 keyLightPosition(4.0f, 6.0f, 3.0f);
    const glm::vec3 keyLightColor(1.0f, 0.97f, 0.92f);

    m_lighting.AddLight(Light::MakePoint(
        keyLightPosition,
        keyLightColor,
        1.0f,
        1.0f,
        0.07f,
        0.017f));

    m_lighting.AddLight(Light::MakeDirectional(
        glm::normalize(glm::vec3(-0.4f, 0.25f, -0.55f)),
        glm::vec3(0.65f, 0.72f, 0.9f),
        0.3f));

    m_lighting.SetIndirectBounceDirection(glm::normalize(keyLightPosition));
    m_lighting.SetIndirectBounceColor(keyLightColor);
}

const SceneLighting& DemoScene::GetLighting() const
{
    return m_lighting;
}

void DemoScene::Update(double deltaTime, bool paused, Input& input)
{
    HandleMovement(input, deltaTime);

    if (!paused)
    {
        m_animationTime += deltaTime;
    }
}

void DemoScene::HandleMovement(Input& input, double deltaTime)
{
    const float moveSpeed = 2.0f;
    const float step = moveSpeed * static_cast<float>(deltaTime);

    if (input.IsKeyDown(GLFW_KEY_LEFT))
    {
        m_position.x -= step;
    }
    if (input.IsKeyDown(GLFW_KEY_RIGHT))
    {
        m_position.x += step;
    }
    if (input.IsKeyDown(GLFW_KEY_UP))
    {
        m_position.z -= step;
    }
    if (input.IsKeyDown(GLFW_KEY_DOWN))
    {
        m_position.z += step;
    }
    if (input.IsKeyDown(GLFW_KEY_PAGE_UP))
    {
        m_position.y += step;
    }
    if (input.IsKeyDown(GLFW_KEY_PAGE_DOWN))
    {
        m_position.y -= step;
    }
}

glm::mat4 DemoScene::BuildModelMatrix() const
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, m_position);
    model = glm::rotate(model, (float)m_animationTime * 1.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, (float)m_animationTime * 0.6f, glm::vec3(1.0f, 0.0f, 0.0f));
    return model;
}

void DemoScene::Render(const Camera& camera, const Material& material) const
{
    m_grid->Draw(camera);

    material.Apply(camera, m_lighting, BuildModelMatrix());
    m_mesh->Draw();
}
