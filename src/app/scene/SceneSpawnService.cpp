#include "app/scene/SceneSpawnService.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneHierarchyOps.h"
#include "app/scene/SceneImportService.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectStore.h"
#include "app/scene/SceneSelectionController.h"
#include "app/scene/SceneSpawnBuilders.h"

#include "engine/components/CameraComponent.h"
#include "engine/rendering/Constants.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/MaterialTextures.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>

namespace
{
    struct PrimitiveSpawnInfo
    {
        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        glm::vec3 position;
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

void SceneSpawnService::SetupDefaultSunLight(Scene& scene)
{
    SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeLightObject(
            "Sun",
            MakeDefaultLightComponent(LightType::Directional),
            MakeDefaultLightTransform(LightType::Directional),
            -1,
            0));
}

void SceneSpawnService::SetupObjects(Scene& scene)
{
    auto floorMaterial = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        glm::vec3(1.0f),
        1.0f,
        0.0f);
    ApplyConcreteFloorMaterialMaps(*floorMaterial);

    SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeRenderableObject(
            "Floor",
            scene.GetMeshLibrary().GetPrimitive(ScenePrimitive::Plane),
            std::move(floorMaterial),
            glm::vec3(-Scene::FloorHalfExtent, -0.01f, -Scene::FloorHalfExtent),
            glm::vec3(Scene::FloorHalfExtent, 0.01f, Scene::FloorHalfExtent),
            Transform{},
            true,
            true,
            -1,
            1));

    auto cubeMaterial = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        glm::vec3(1.0f),
        0.85f,
        0.0f);
    ApplyWoodTableMaterialMaps(*cubeMaterial);

    Transform cubeTransform;
    cubeTransform.position = glm::vec3(0.0f, 1.5f, 0.0f);

    SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeRenderableObject(
            "Cube",
            scene.GetMeshLibrary().GetPrimitive(ScenePrimitive::Cube),
            std::move(cubeMaterial),
            glm::vec3(-0.5f),
            glm::vec3(0.5f),
            cubeTransform,
            true,
            true,
            -1,
            2));

    scene.GetSelectionController().SelectSingle(scene.GetObjectStore().Objects(), 2);
}

void SceneSpawnService::ResetToDefault(Scene& scene)
{
    scene.GetObjectStore().Clear();
    scene.GetMeshLibrary().ClearImportedMeshes();
    scene.GetSelectionController().Clear();
    scene.GetObjectStore().SetNextId(1);
    scene.SetShowLightGizmos(true);
    scene.SetShowGrid(true);
    m_nextDirectionalLightNumber = 2;
    m_nextPointLightNumber = 1;
    m_nextSpotLightNumber = 1;
    m_nextCubeNumber = 2;
    m_nextSphereNumber = 1;
    m_nextCylinderNumber = 1;
    m_nextCapsuleNumber = 1;
    m_nextPlaneNumber = 1;
    m_nextEmptyNumber = 1;
    m_nextImportNumber = 1;
    scene.GetImportService().ClearMessages();

    SetupDefaultSunLight(scene);
    SetupObjects(scene);
}

int SceneSpawnService::GetNextObjectNumber(ScenePrimitive primitive)
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

int SceneSpawnService::AddObject(Scene& scene, ScenePrimitive primitive, int parentIndex)
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

    return SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeRenderableObject(
            objectName,
            scene.GetMeshLibrary().GetPrimitive(primitive),
            CreateDefaultObjectMaterial(),
            spawnInfo.boundsMin,
            spawnInfo.boundsMax,
            transform,
            spawnInfo.castShadow,
            spawnInfo.receiveShadow,
            parentIndex,
            SceneHierarchyOps::AllocateSiblingOrder(scene.GetObjectStore().Objects(), parentIndex)));
}

int SceneSpawnService::AddEmptyObject(Scene& scene, int parentIndex)
{
    const std::string objectName = "Empty " + std::to_string(m_nextEmptyNumber++);

    return SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeEmptyObject(
            objectName,
            parentIndex,
            SceneHierarchyOps::AllocateSiblingOrder(scene.GetObjectStore().Objects(), parentIndex)));
}

int SceneSpawnService::AddLightObject(Scene& scene, LightType type, int parentIndex)
{
    LightComponent lightComponent = MakeDefaultLightComponent(type);
    Transform transform = MakeDefaultLightTransform(type);

    if (lightComponent.castsShadow)
    {
        for (const SceneObject& object : scene.GetObjectStore().Objects())
        {
            if (object.HasLight() && object.GetLight().castsShadow)
            {
                lightComponent.castsShadow = false;
                break;
            }
        }
    }

    std::string objectName;
    switch (type)
    {
    case LightType::Directional:
        objectName = "Directional Light " + std::to_string(m_nextDirectionalLightNumber++);
        break;
    case LightType::Point:
        objectName = "Point Light " + std::to_string(m_nextPointLightNumber++);
        break;
    case LightType::Spot:
        objectName = "Spot Light " + std::to_string(m_nextSpotLightNumber++);
        break;
    }

    return SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeLightObject(
            objectName,
            std::move(lightComponent),
            transform,
            parentIndex,
            SceneHierarchyOps::AllocateSiblingOrder(scene.GetObjectStore().Objects(), parentIndex)));
}

int SceneSpawnService::AddCameraObject(Scene& scene, int parentIndex)
{
    CameraComponent cameraComponent = MakeDefaultCameraComponent();
    Transform transform = MakeDefaultCameraTransform();

    bool hasMainCamera = false;
    for (const SceneObject& object : scene.GetObjectStore().Objects())
    {
        if (object.HasCamera() && object.GetCamera().isMain)
        {
            hasMainCamera = true;
            break;
        }
    }

    if (!hasMainCamera)
    {
        cameraComponent.isMain = true;
    }

    const std::string objectName = "Camera " + std::to_string(m_nextCameraNumber++);

    return SceneSpawnBuilders::AppendObject(
        scene,
        SceneSpawnBuilders::MakeCameraObject(
            objectName,
            std::move(cameraComponent),
            transform,
            parentIndex,
            SceneHierarchyOps::AllocateSiblingOrder(scene.GetObjectStore().Objects(), parentIndex)));
}

void SceneSpawnService::EnsureUniqueMainCamera(Scene& scene, int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjectStore().Objects().size()))
    {
        return;
    }

    SceneObject& object = scene.GetObjectStore().Objects()[static_cast<std::size_t>(objectIndex)];
    if (!object.HasCamera() || !object.GetCamera().isMain)
    {
        return;
    }

    for (std::size_t index = 0; index < scene.GetObjectStore().Objects().size(); ++index)
    {
        if (static_cast<int>(index) == objectIndex)
        {
            continue;
        }

        SceneObject& otherObject = scene.GetObjectStore().Objects()[index];
        if (otherObject.HasCamera() && otherObject.GetCamera().isMain)
        {
            otherObject.GetCamera().isMain = false;
        }
    }
}

int SceneSpawnService::AllocateImportNumber()
{
    return m_nextImportNumber++;
}

void SceneSpawnService::ResetCounters()
{
    m_nextDirectionalLightNumber = 2;
    m_nextPointLightNumber = 1;
    m_nextSpotLightNumber = 1;
    m_nextCubeNumber = 2;
    m_nextSphereNumber = 1;
    m_nextCylinderNumber = 1;
    m_nextCapsuleNumber = 1;
    m_nextPlaneNumber = 1;
    m_nextEmptyNumber = 1;
    m_nextCameraNumber = 1;
    m_nextImportNumber = 1;
}

SceneSpawnCounters SceneSpawnService::GetCounters() const
{
    return SceneSpawnCounters{
        m_nextDirectionalLightNumber,
        m_nextPointLightNumber,
        m_nextSpotLightNumber,
        m_nextCubeNumber,
        m_nextSphereNumber,
        m_nextCylinderNumber,
        m_nextCapsuleNumber,
        m_nextPlaneNumber,
        m_nextEmptyNumber,
        m_nextCameraNumber,
        m_nextImportNumber,
    };
}

void SceneSpawnService::SetCounters(const SceneSpawnCounters& counters)
{
    m_nextDirectionalLightNumber = counters.directionalLight;
    m_nextPointLightNumber = counters.pointLight;
    m_nextSpotLightNumber = counters.spotLight;
    m_nextCubeNumber = counters.cube;
    m_nextSphereNumber = counters.sphere;
    m_nextCylinderNumber = counters.cylinder;
    m_nextCapsuleNumber = counters.capsule;
    m_nextPlaneNumber = counters.plane;
    m_nextEmptyNumber = counters.empty;
    m_nextCameraNumber = counters.camera;
    m_nextImportNumber = counters.import;
}
