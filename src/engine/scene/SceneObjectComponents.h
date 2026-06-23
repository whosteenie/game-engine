#pragma once

#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidBodyComponent.h"

#include <memory>
#include <optional>
#include <vector>

class Material;
class SceneObject;

struct SceneObjectComponentSnapshot
{
    std::unique_ptr<Material> material;
    std::optional<LightComponent> light;
    std::optional<CameraComponent> camera;
    std::optional<RigidBodyComponent> rigidBody;
    std::optional<ColliderComponent> collider;
    std::vector<InspectorComponentType> inspectorComponentOrder;
};

SceneObjectComponentSnapshot CaptureSceneObjectComponents(const SceneObject& source);
void ApplySceneObjectInspectorOrder(SceneObject& object, const std::vector<InspectorComponentType>& order);
