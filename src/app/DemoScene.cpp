#include "app/DemoScene.h"

#include "app/SceneEditor.h"
#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/MaterialTextures.h"
#include "engine/Mesh.h"
#include "primitives/Cube.h"
#include "primitives/Floor.h"
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

DemoScene::DemoScene()
    : m_cubeMesh(CreateCubeMesh()),
      m_sphereMesh(CreateSphereMesh()),
      m_cylinderMesh(CreateCylinderMesh()),
      m_capsuleMesh(CreateCapsuleMesh()),
      m_planeMesh(CreatePlaneMesh()),
      m_floorMesh(CreateFloorMesh(FloorHalfExtent)),
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

void DemoScene::SetupObjects()
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
        m_floorMesh.get(),
        std::move(floorMaterial),
        glm::vec3(-FloorHalfExtent, -0.05f, -FloorHalfExtent),
        glm::vec3(FloorHalfExtent, 0.05f, FloorHalfExtent),
        Transform{},
        false,
        false,
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
                glm::vec3(-5.0f, -0.01f, -5.0f),
                glm::vec3(5.0f, 0.01f, 5.0f),
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

Mesh* DemoScene::GetMeshForPrimitive(ScenePrimitive primitive)
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

int DemoScene::GetNextObjectNumber(ScenePrimitive primitive)
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

int DemoScene::AddObject(ScenePrimitive primitive)
{
    const int instanceNumber = GetNextObjectNumber(primitive);
    const PrimitiveSpawnInfo spawnInfo = GetPrimitiveSpawnInfo(primitive, instanceNumber);

    const std::string objectName =
        std::string(GetScenePrimitiveDisplayName(primitive)) + " " + std::to_string(instanceNumber);

    Transform transform;
    transform.position = spawnInfo.position;

    m_objects.emplace_back(
        objectName,
        GetMeshForPrimitive(primitive),
        CreateDefaultObjectMaterial(),
        spawnInfo.boundsMin,
        spawnInfo.boundsMax,
        transform,
        spawnInfo.movable,
        spawnInfo.castShadow,
        spawnInfo.receiveShadow);

    return static_cast<int>(m_objects.size()) - 1;
}

bool DemoScene::RemoveObject(std::size_t index)
{
    if (index >= m_objects.size())
    {
        return false;
    }

    m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(index));

    if (m_objects.empty())
    {
        m_selectedObjectIndex = -1;
        return true;
    }

    if (m_selectedObjectIndex == static_cast<int>(index))
    {
        m_selectedObjectIndex = std::min(
            static_cast<int>(index),
            static_cast<int>(m_objects.size()) - 1);
    }
    else if (m_selectedObjectIndex > static_cast<int>(index))
    {
        --m_selectedObjectIndex;
    }

    return true;
}

const SceneLighting& DemoScene::GetLighting() const
{
    return m_lighting;
}

SceneLighting& DemoScene::GetLighting()
{
    return m_lighting;
}

IBL& DemoScene::GetIBL()
{
    return *m_ibl;
}

const std::vector<SceneObject>& DemoScene::GetObjects() const
{
    return m_objects;
}

std::vector<SceneObject>& DemoScene::GetObjects()
{
    return m_objects;
}

SceneObject& DemoScene::GetObject(std::size_t index)
{
    return m_objects.at(index);
}

const SceneObject& DemoScene::GetObject(std::size_t index) const
{
    return m_objects.at(index);
}

int DemoScene::GetSelectedObjectIndex() const
{
    return m_selectedObjectIndex;
}

void DemoScene::SetSelectedObjectIndex(int selectedObjectIndex)
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

void DemoScene::ClearSelection()
{
    m_selectedObjectIndex = -1;
}

bool DemoScene::HasSelection() const
{
    return m_selectedObjectIndex >= 0 && static_cast<std::size_t>(m_selectedObjectIndex) < m_objects.size();
}

SceneEditor& DemoScene::GetSceneEditor()
{
    return *m_sceneEditor;
}

const SceneEditor& DemoScene::GetSceneEditor() const
{
    return *m_sceneEditor;
}

bool DemoScene::GetShowLightGizmos() const
{
    return m_showLightGizmos;
}

void DemoScene::SetShowLightGizmos(bool showLightGizmos)
{
    m_showLightGizmos = showLightGizmos;
}

int DemoScene::GetSelectedLightIndex() const
{
    return m_selectedLightIndex;
}

void DemoScene::SetSelectedLightIndex(int selectedLightIndex)
{
    if (selectedLightIndex < 0)
    {
        m_selectedLightIndex = 0;
        return;
    }

    if (static_cast<std::size_t>(selectedLightIndex) >= m_lighting.GetLightCount())
    {
        m_selectedLightIndex = static_cast<int>(m_lighting.GetLightCount()) - 1;
        return;
    }

    m_selectedLightIndex = selectedLightIndex;
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

void DemoScene::Update(
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

void DemoScene::RenderShadowPass() const
{
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());

    for (const SceneObject& object : m_objects)
    {
        if (!object.CastsShadow() && !object.ReceivesShadow())
        {
            continue;
        }

        glm::vec3 objectBoundsMin;
        glm::vec3 objectBoundsMax;
        object.GetWorldBounds(objectBoundsMin, objectBoundsMax);
        boundsMin = glm::min(boundsMin, objectBoundsMin);
        boundsMax = glm::max(boundsMax, objectBoundsMax);
    }

    m_shadowMap->BeginPass(GetSunDirection(), boundsMin, boundsMax);

    m_shadowDepthShader->Use();
    m_shadowDepthShader->SetMat4("uLightSpaceMatrix", m_shadowMap->GetLightSpaceMatrix());

    for (const SceneObject& object : m_objects)
    {
        if (!object.CastsShadow())
        {
            continue;
        }

        m_shadowDepthShader->SetMat4("uModel", object.BuildModelMatrix());
        object.GetMesh()->Draw();
    }
}

void DemoScene::Render(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) const
{
    RenderShadowPass();
    m_shadowMap->EndPass();

    glViewport(0, 0, viewportWidth, viewportHeight);

    for (const SceneObject& object : m_objects)
    {
        const glm::mat4 modelMatrix = object.BuildModelMatrix();
        object.GetMaterial().Apply(
            camera,
            m_lighting,
            *m_ibl,
            modelMatrix,
            m_shadowMap.get(),
            object.ReceivesShadow());
        object.GetMesh()->Draw();
    }

    m_grid->Draw(camera);

    if (m_showLightGizmos)
    {
        m_lightGizmos->Draw(camera, m_lighting, m_selectedLightIndex);
    }
}
