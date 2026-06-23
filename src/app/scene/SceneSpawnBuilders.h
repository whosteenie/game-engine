#pragma once

#include "engine/components/CameraComponent.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/Material.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/Transform.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>

class Mesh;
class Scene;

namespace SceneSpawnBuilders
{
    int AppendObject(Scene& scene, SceneObject object);

    SceneObject MakeLightObject(
        std::string name,
        LightComponent light,
        Transform transform,
        int parentIndex,
        int siblingOrder);

    SceneObject MakeCameraObject(
        std::string name,
        CameraComponent camera,
        Transform transform,
        int parentIndex,
        int siblingOrder);

    SceneObject MakeEmptyObject(
        std::string name,
        int parentIndex,
        int siblingOrder);

    SceneObject MakeRenderableObject(
        std::string name,
        Mesh* mesh,
        std::unique_ptr<Material> material,
        const glm::vec3& localBoundsMin,
        const glm::vec3& localBoundsMax,
        Transform transform,
        bool castShadow,
        bool receiveShadow,
        int parentIndex,
        int siblingOrder);
}
