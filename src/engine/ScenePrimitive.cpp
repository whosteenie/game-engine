#include "engine/ScenePrimitive.h"

const char* GetScenePrimitiveDisplayName(ScenePrimitive primitive)
{
    switch (primitive)
    {
    case ScenePrimitive::Cube:
        return "Cube";
    case ScenePrimitive::Sphere:
        return "Sphere";
    case ScenePrimitive::Cylinder:
        return "Cylinder";
    case ScenePrimitive::Capsule:
        return "Capsule";
    case ScenePrimitive::Plane:
        return "Plane";
    }

    return "Object";
}
