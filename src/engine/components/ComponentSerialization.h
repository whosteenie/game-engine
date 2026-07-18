#pragma once

#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/rendering/resources/Material.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

using MaterialStoredPathFn = std::function<std::string(const std::string& path)>;
using MaterialResolvePathFn = std::function<std::string(const std::string& storedPath)>;

nlohmann::json LightComponentToJson(const LightComponent& light);
LightComponent LightComponentFromJson(const nlohmann::json& value);

nlohmann::json CameraComponentToJson(const CameraComponent& camera);
CameraComponent CameraComponentFromJson(const nlohmann::json& value);

nlohmann::json ColliderComponentToJson(const ColliderComponent& collider);
ColliderComponent ColliderComponentFromJson(const nlohmann::json& value);

nlohmann::json RigidBodyComponentToJson(const RigidBodyComponent& rigidBody);
RigidBodyComponent RigidBodyComponentFromJson(const nlohmann::json& value);

nlohmann::json MaterialToJson(const Material& material, const MaterialStoredPathFn& toStoredPath);
std::unique_ptr<Material> MaterialFromJson(
    const nlohmann::json& value,
    const MaterialResolvePathFn& resolvePath,
    const MaterialStoredPathFn& toStoredPath);
