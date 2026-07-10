#include "engine/components/RigidBodyComponent.h"

#include "engine/components/ComponentCompare.h"

#include <nlohmann/json.hpp>

#include <cstring>

using json = nlohmann::json;

const char* RigidBodyCollisionDetectionToString(const RigidBodyCollisionDetection mode)
{
    switch (mode)
    {
    case RigidBodyCollisionDetection::Continuous:
        return "continuous";
    case RigidBodyCollisionDetection::Discrete:
    default:
        return "discrete";
    }
}

bool RigidBodyCollisionDetectionFromString(const char* value, RigidBodyCollisionDetection& outMode)
{
    if (value == nullptr)
    {
        return false;
    }

    if (std::strcmp(value, "continuous") == 0)
    {
        outMode = RigidBodyCollisionDetection::Continuous;
        return true;
    }

    if (std::strcmp(value, "discrete") == 0)
    {
        outMode = RigidBodyCollisionDetection::Discrete;
        return true;
    }

    return false;
}

bool operator==(const RigidBodyComponent& left, const RigidBodyComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return FloatsEqual(left.mass, right.mass)
        && left.useGravity == right.useGravity
        && left.isKinematic == right.isKinematic
        && left.collisionDetection == right.collisionDetection
        && FloatsEqual(left.linearDamping, right.linearDamping)
        && FloatsEqual(left.angularDamping, right.angularDamping)
        && left.allowSleeping == right.allowSleeping;
}

json RigidBodyComponentToJson(const RigidBodyComponent& rigidBody)
{
    return json{
        {"mass", rigidBody.mass},
        {"useGravity", rigidBody.useGravity},
        {"isKinematic", rigidBody.isKinematic},
        {"collisionDetection", RigidBodyCollisionDetectionToString(rigidBody.collisionDetection)},
        {"linearDamping", rigidBody.linearDamping},
        {"angularDamping", rigidBody.angularDamping},
        {"allowSleeping", rigidBody.allowSleeping},
    };
}

RigidBodyComponent RigidBodyComponentFromJson(const json& value)
{
    RigidBodyComponent rigidBody = MakeDefaultRigidBodyComponent();
    rigidBody.mass = value.value("mass", rigidBody.mass);
    rigidBody.useGravity = value.value("useGravity", rigidBody.useGravity);
    rigidBody.isKinematic = value.value("isKinematic", rigidBody.isKinematic);
    if (value.contains("collisionDetection"))
    {
        RigidBodyCollisionDetection parsedMode = rigidBody.collisionDetection;
        if (RigidBodyCollisionDetectionFromString(
                value.at("collisionDetection").get<std::string>().c_str(),
                parsedMode))
        {
            rigidBody.collisionDetection = parsedMode;
        }
    }
    rigidBody.linearDamping = value.value("linearDamping", rigidBody.linearDamping);
    rigidBody.angularDamping = value.value("angularDamping", rigidBody.angularDamping);
    rigidBody.allowSleeping = value.value("allowSleeping", rigidBody.allowSleeping);
    return rigidBody;
}

RigidBodyComponent MakeDefaultRigidBodyComponent()
{
    return RigidBodyComponent{};
}
