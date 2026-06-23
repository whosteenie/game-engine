#include "app/SceneComponentCatalog.h"

#include "app/Scene.h"
#include "engine/CameraComponent.h"
#include "engine/ColliderComponent.h"
#include "engine/LightComponent.h"
#include "engine/RigidBodyComponent.h"
#include "engine/SceneObject.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kMinColliderHalfExtent = 0.05f;

    ColliderComponent BuildSmartDefaultCollider(const SceneObject& object)
    {
        ColliderComponent collider = MakeDefaultColliderComponent();
        if (!object.HasMesh())
        {
            return collider;
        }

        const glm::vec3 boundsMin = object.GetLocalBoundsMin();
        const glm::vec3 boundsMax = object.GetLocalBoundsMax();
        glm::vec3 halfExtents = glm::abs(boundsMax - boundsMin) * 0.5f;

        const bool validExtents =
            std::isfinite(halfExtents.x) && std::isfinite(halfExtents.y) && std::isfinite(halfExtents.z);
        if (!validExtents)
        {
            return collider;
        }

        halfExtents = glm::max(halfExtents, glm::vec3(kMinColliderHalfExtent));
        collider.offset = (boundsMin + boundsMax) * 0.5f;
        collider.halfExtents = halfExtents;
        collider.radius = std::max(
            kMinColliderHalfExtent,
            std::max(halfExtents.x, std::max(halfExtents.y, halfExtents.z)));
        return collider;
    }
}

const char* GetSceneSystemComponentLabel(SceneSystemComponentType type)
{
    switch (type)
    {
    case SceneSystemComponentType::Light:
        return "Light";
    case SceneSystemComponentType::Camera:
        return "Camera";
    case SceneSystemComponentType::RigidBody:
        return "Rigid Body";
    case SceneSystemComponentType::Collider:
        return "Collider";
    }

    return "Unknown";
}

bool SceneObjectHasSystemComponent(const SceneObject& object, SceneSystemComponentType type)
{
    switch (type)
    {
    case SceneSystemComponentType::Light:
        return object.HasLight();
    case SceneSystemComponentType::Camera:
        return object.HasCamera();
    case SceneSystemComponentType::RigidBody:
        return object.HasRigidBody();
    case SceneSystemComponentType::Collider:
        return object.HasCollider();
    }

    return false;
}

bool CanAddSceneSystemComponent(const SceneObject& object, SceneSystemComponentType type)
{
    return !SceneObjectHasSystemComponent(object, type);
}

void GetAddableSceneSystemComponents(const SceneObject& object, std::vector<SceneSystemComponentType>& out)
{
    out.clear();

    constexpr SceneSystemComponentType kAllComponents[] = {
        SceneSystemComponentType::Light,
        SceneSystemComponentType::Camera,
        SceneSystemComponentType::RigidBody,
        SceneSystemComponentType::Collider,
    };

    for (SceneSystemComponentType type : kAllComponents)
    {
        if (CanAddSceneSystemComponent(object, type))
        {
            out.push_back(type);
        }
    }
}

void AddSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjects().size()))
    {
        return;
    }

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
    if (!CanAddSceneSystemComponent(object, type))
    {
        return;
    }

    switch (type)
    {
    case SceneSystemComponentType::Light:
        object.SetLight(MakeDefaultLightComponent(LightType::Point));
        break;
    case SceneSystemComponentType::Camera:
    {
        CameraComponent camera = MakeDefaultCameraComponent();
        bool hasMainCamera = false;
        for (const SceneObject& otherObject : scene.GetObjects())
        {
            if (otherObject.HasCamera() && otherObject.GetCamera().isMain)
            {
                hasMainCamera = true;
                break;
            }
        }

        if (!hasMainCamera)
        {
            camera.isMain = true;
        }

        object.SetCamera(std::move(camera));
        break;
    }
    case SceneSystemComponentType::RigidBody:
        object.SetRigidBody(MakeDefaultRigidBodyComponent());
        break;
    case SceneSystemComponentType::Collider:
        object.SetCollider(BuildSmartDefaultCollider(object));
        break;
    }

    scene.MarkDirty();
}

void RemoveSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjects().size()))
    {
        return;
    }

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
    if (!SceneObjectHasSystemComponent(object, type))
    {
        return;
    }

    switch (type)
    {
    case SceneSystemComponentType::Light:
        object.ClearLight();
        break;
    case SceneSystemComponentType::Camera:
        object.ClearCamera();
        break;
    case SceneSystemComponentType::RigidBody:
        object.ClearRigidBody();
        break;
    case SceneSystemComponentType::Collider:
        object.ClearCollider();
        break;
    }

    scene.MarkDirty();
}
