#include "app/DemoScene.h"

#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/Mesh.h"
#include "primitives/Cube.h"
#include "primitives/Floor.h"
#include "engine/GridRenderer.h"
#include "engine/SceneLighting.h"
#include "engine/ShadowMap.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

DemoScene::DemoScene()
    : m_cubeMesh(CreateCubeMesh()),
      m_floorMesh(CreateFloorMesh(FloorHalfExtent)),
      m_grid(std::make_unique<GridRenderer>()),
      m_shadowMap(std::make_unique<ShadowMap>()),
      m_ibl(std::make_unique<IBL>(EngineConstants::EnvironmentHdr)),
      m_shadowDepthShader(std::make_unique<Shader>(
          EngineConstants::ShadowDepthVertexShader,
          EngineConstants::ShadowDepthFragmentShader)),
      m_floorMaterial(std::make_unique<Material>(
          EngineConstants::LitVertexShader,
          EngineConstants::PbrFragmentShader,
          glm::vec3(0.12f, 0.12f, 0.14f),
          0.9f,
          0.0f))
{
    SetupLighting();
}

DemoScene::~DemoScene() = default;

void DemoScene::SetupLighting()
{
    const glm::vec3 sunDirection = glm::normalize(glm::vec3(0.45f, 0.7f, 0.55f));
    const glm::vec3 sunColor(1.0f, 0.97f, 0.92f);

    m_lighting.AddLight(Light::MakeDirectional(
        sunDirection,
        sunColor,
        2.5f));

    m_lighting.SetShadowLightIndex(0);
}

const SceneLighting& DemoScene::GetLighting() const
{
    return m_lighting;
}

glm::vec3 DemoScene::GetSunDirection() const
{
    const auto& lights = m_lighting.GetLights();
    if (lights.empty())
    {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }

    return lights.front().GetDirection();
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

glm::mat4 DemoScene::BuildCubeModelMatrix() const
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, m_position);
    model = glm::rotate(model, (float)m_animationTime * 1.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, (float)m_animationTime * 0.6f, glm::vec3(1.0f, 0.0f, 0.0f));
    return model;
}

glm::mat4 DemoScene::BuildFloorModelMatrix() const
{
    return glm::mat4(1.0f);
}

void DemoScene::RenderShadowPass(const glm::mat4& cubeModel) const
{
    m_shadowMap->BeginPass(GetSunDirection(), glm::vec3(0.0f));

    m_shadowDepthShader->Use();
    m_shadowDepthShader->SetMat4("uLightSpaceMatrix", m_shadowMap->GetLightSpaceMatrix());
    m_shadowDepthShader->SetMat4("uModel", cubeModel);
    m_cubeMesh->Draw();
}

void DemoScene::Render(
    const Camera& camera,
    const Material& cubeMaterial,
    int viewportWidth,
    int viewportHeight) const
{
    const glm::mat4 cubeModel = BuildCubeModelMatrix();
    const glm::mat4 floorModel = BuildFloorModelMatrix();

    RenderShadowPass(cubeModel);
    m_shadowMap->EndPass();

    glViewport(0, 0, viewportWidth, viewportHeight);

    m_floorMaterial->Apply(camera, m_lighting, *m_ibl, floorModel, m_shadowMap.get(), true);
    m_floorMesh->Draw();

    m_grid->Draw(camera);

    cubeMaterial.Apply(camera, m_lighting, *m_ibl, cubeModel, m_shadowMap.get(), false);
    m_cubeMesh->Draw();
}
