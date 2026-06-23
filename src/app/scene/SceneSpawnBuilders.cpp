#include "app/scene/SceneSpawnBuilders.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectStore.h"

int SceneSpawnBuilders::AppendObject(Scene& scene, SceneObject object)
{
    scene.GetObjectStore().Objects().emplace_back(std::move(object));
    scene.GetObjectStore().FinalizeNewObject(scene.GetObjectStore().Objects().back());
    scene.MarkDirty();
    return static_cast<int>(scene.GetObjectStore().Objects().size()) - 1;
}

SceneObject SceneSpawnBuilders::MakeLightObject(
    std::string name,
    LightComponent light,
    Transform transform,
    const int parentIndex,
    const int siblingOrder)
{
    return SceneObject(
        std::move(name),
        nullptr,
        nullptr,
        glm::vec3(-0.15f),
        glm::vec3(0.15f),
        std::move(transform),
        false,
        false,
        parentIndex,
        siblingOrder,
        std::move(light));
}

SceneObject SceneSpawnBuilders::MakeCameraObject(
    std::string name,
    CameraComponent camera,
    Transform transform,
    const int parentIndex,
    const int siblingOrder)
{
    return SceneObject(
        std::move(name),
        nullptr,
        nullptr,
        glm::vec3(-0.15f),
        glm::vec3(0.15f),
        std::move(transform),
        false,
        false,
        parentIndex,
        siblingOrder,
        std::nullopt,
        std::move(camera));
}

SceneObject SceneSpawnBuilders::MakeEmptyObject(
    std::string name,
    const int parentIndex,
    const int siblingOrder)
{
    return SceneObject(
        std::move(name),
        nullptr,
        nullptr,
        glm::vec3(0.0f),
        glm::vec3(0.0f),
        Transform{},
        false,
        false,
        parentIndex,
        siblingOrder);
}

SceneObject SceneSpawnBuilders::MakeRenderableObject(
    std::string name,
    Mesh* mesh,
    std::unique_ptr<Material> material,
    const glm::vec3& localBoundsMin,
    const glm::vec3& localBoundsMax,
    Transform transform,
    const bool castShadow,
    const bool receiveShadow,
    const int parentIndex,
    const int siblingOrder)
{
    return SceneObject(
        std::move(name),
        mesh,
        std::move(material),
        localBoundsMin,
        localBoundsMax,
        std::move(transform),
        castShadow,
        receiveShadow,
        parentIndex,
        siblingOrder);
}
