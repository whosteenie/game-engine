#include "engine/scene/inspector/ComponentOrderJson.h"

#include "engine/scene/SceneObject.h"

#include <nlohmann/json.hpp>

nlohmann::json InspectorComponentOrderToJson(const SceneObject& object)
{
    nlohmann::json order = nlohmann::json::array();
    for (const InspectorComponentType type : object.GetInspectorComponentOrder())
    {
        order.push_back(GetInspectorComponentJsonKey(type));
    }

    return order;
}

std::vector<InspectorComponentType> InspectorComponentOrderFromJson(const nlohmann::json& value)
{
    std::vector<InspectorComponentType> order;
    if (!value.is_array())
    {
        return order;
    }

    for (const nlohmann::json& entry : value)
    {
        if (!entry.is_string())
        {
            continue;
        }

        InspectorComponentType type = InspectorComponentType::Material;
        if (InspectorComponentTypeFromJsonKey(entry.get<std::string>(), type))
        {
            order.push_back(type);
        }
    }

    return order;
}
