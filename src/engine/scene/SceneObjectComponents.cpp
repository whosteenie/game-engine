#include "engine/scene/SceneObjectComponents.h"

#include "engine/rendering/Material.h"
#include "engine/scene/SceneObject.h"

SceneObjectComponentSnapshot CaptureSceneObjectComponents(const SceneObject& source)
{
    SceneObjectComponentSnapshot snapshot;
    if (source.HasMaterial())
    {
        snapshot.material = source.GetMaterial().Clone();
    }

    if (source.HasLight())
    {
        snapshot.light = source.GetLight();
    }

    if (source.HasCamera())
    {
        snapshot.camera = source.GetCamera();
    }

    if (source.HasRigidBody())
    {
        snapshot.rigidBody = source.GetRigidBody();
    }

    if (source.HasCollider())
    {
        snapshot.collider = source.GetCollider();
    }

    snapshot.inspectorComponentOrder = source.GetInspectorComponentOrder();
    return snapshot;
}

void ApplySceneObjectInspectorOrder(SceneObject& object, const std::vector<InspectorComponentType>& order)
{
    object.SetInspectorComponentOrder(order);
}
