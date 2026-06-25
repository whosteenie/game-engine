#include "app/scene/SceneComponentCatalog.h"

#include "app/scene/Scene.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObject.h"

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

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
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

    std::vector<InspectorComponentType> order = object.GetEffectiveInspectorComponentOrder();
    AppendInspectorComponentType(order, InspectorComponentTypeFromSystem(type));
    object.SetInspectorComponentOrder(std::move(order));

    scene.MarkDirty();
}

void RemoveSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type)
{
    if (objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjects().size()))
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
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

    std::vector<InspectorComponentType> order = object.GetInspectorComponentOrder();
    if (order.empty())
    {
        order = object.GetEffectiveInspectorComponentOrder();
    }

    RemoveInspectorComponentType(order, InspectorComponentTypeFromSystem(type));
    if (type != SceneSystemComponentType::Light
        && SceneObjectHasInspectorComponent(object, InspectorComponentType::ObjectFlags))
    {
        AppendInspectorComponentType(order, InspectorComponentType::ObjectFlags);
    }

    object.SetInspectorComponentOrder(std::move(order));

    scene.MarkDirty();
}
