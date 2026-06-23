#include "engine/components/RigidBodyComponent.h"

#include "engine/components/ComponentCompare.h"

bool operator==(const RigidBodyComponent& left, const RigidBodyComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return FloatsEqual(left.mass, right.mass)
        && left.useGravity == right.useGravity
        && left.isKinematic == right.isKinematic;
}

RigidBodyComponent MakeDefaultRigidBodyComponent()
{
    return RigidBodyComponent{};
}
