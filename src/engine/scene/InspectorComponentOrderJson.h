#pragma once

#include "engine/scene/InspectorComponentOrder.h"

#include <nlohmann/json.hpp>
#include <vector>

class SceneObject;

nlohmann::json InspectorComponentOrderToJson(const SceneObject& object);
std::vector<InspectorComponentType> InspectorComponentOrderFromJson(const nlohmann::json& value);
