#include "engine/ScenePrimitive.h"

#include <string>

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

const char* ScenePrimitiveToString(ScenePrimitive primitive)
{
    return GetScenePrimitiveDisplayName(primitive);
}

bool TryParseScenePrimitive(const char* name, ScenePrimitive& outPrimitive)
{
    if (name == nullptr)
    {
        return false;
    }

    const ScenePrimitive primitives[] = {
        ScenePrimitive::Cube,
        ScenePrimitive::Sphere,
        ScenePrimitive::Cylinder,
        ScenePrimitive::Capsule,
        ScenePrimitive::Plane,
    };

    for (ScenePrimitive primitive : primitives)
    {
        if (std::string(ScenePrimitiveToString(primitive)) == name)
        {
            outPrimitive = primitive;
            return true;
        }
    }

    return false;
}
