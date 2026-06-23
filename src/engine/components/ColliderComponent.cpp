#include "engine/components/ColliderComponent.h"

#include "engine/components/ComponentCompare.h"

bool operator==(const ColliderComponent& left, const ColliderComponent& right)
{
    using ComponentCompare::FloatsEqual;

    return left.shape == right.shape
        && left.offset == right.offset
        && left.halfExtents == right.halfExtents
        && FloatsEqual(left.radius, right.radius)
        && left.isTrigger == right.isTrigger;
}

ColliderComponent MakeDefaultColliderComponent()
{
    return ColliderComponent{};
}
