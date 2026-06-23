#include "engine/components/ColliderComponent.h"

#include "engine/components/ComponentCompare.h"
#include "engine/scene/JsonMath.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

const char* ColliderShapeToString(const ColliderShape shape)
{
    switch (shape)
    {
    case ColliderShape::Box:
        return "box";
    case ColliderShape::Sphere:
        return "sphere";
    }

    return "box";
}

bool ColliderShapeFromString(const std::string& value, ColliderShape& outShape)
{
    if (value == "box")
    {
        outShape = ColliderShape::Box;
        return true;
    }

    if (value == "sphere")
    {
        outShape = ColliderShape::Sphere;
        return true;
    }

    return false;
}

bool operator==(const ColliderComponent& left, const ColliderComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return left.shape == right.shape
        && left.offset == right.offset
        && left.halfExtents == right.halfExtents
        && FloatsEqual(left.radius, right.radius)
        && left.isTrigger == right.isTrigger;
}

json ColliderComponentToJson(const ColliderComponent& collider)
{
    return json{
        {"shape", ColliderShapeToString(collider.shape)},
        {"offset", Vec3ToJson(collider.offset)},
        {"halfExtents", Vec3ToJson(collider.halfExtents)},
        {"radius", collider.radius},
        {"isTrigger", collider.isTrigger},
    };
}

ColliderComponent ColliderComponentFromJson(const json& value)
{
    ColliderComponent collider = MakeDefaultColliderComponent();
    ColliderShape shape = ColliderShape::Box;
    if (value.contains("shape"))
    {
        ColliderShapeFromString(value.at("shape").get<std::string>(), shape);
    }

    collider.shape = shape;
    if (value.contains("offset"))
    {
        collider.offset = Vec3FromJson(value.at("offset"));
    }
    if (value.contains("halfExtents"))
    {
        collider.halfExtents = Vec3FromJson(value.at("halfExtents"));
    }
    collider.radius = value.value("radius", collider.radius);
    collider.isTrigger = value.value("isTrigger", collider.isTrigger);
    return collider;
}

ColliderComponent MakeDefaultColliderComponent()
{
    return ColliderComponent{};
}
