#include "engine/components/RigidBodyComponent.h"

#include "engine/components/ComponentCompare.h"
#include "engine/scene/JsonMath.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool operator==(const RigidBodyComponent& left, const RigidBodyComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return FloatsEqual(left.mass, right.mass)
        && left.useGravity == right.useGravity
        && left.isKinematic == right.isKinematic;
}

json RigidBodyComponentToJson(const RigidBodyComponent& rigidBody)
{
    return json{
        {"mass", rigidBody.mass},
        {"useGravity", rigidBody.useGravity},
        {"isKinematic", rigidBody.isKinematic},
    };
}

RigidBodyComponent RigidBodyComponentFromJson(const json& value)
{
    RigidBodyComponent rigidBody = MakeDefaultRigidBodyComponent();
    rigidBody.mass = value.value("mass", rigidBody.mass);
    rigidBody.useGravity = value.value("useGravity", rigidBody.useGravity);
    rigidBody.isKinematic = value.value("isKinematic", rigidBody.isKinematic);
    return rigidBody;
}

RigidBodyComponent MakeDefaultRigidBodyComponent()
{
    return RigidBodyComponent{};
}
