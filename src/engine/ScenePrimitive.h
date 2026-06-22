#pragma once

enum class ScenePrimitive
{
    Cube,
    Sphere,
    Cylinder,
    Capsule,
    Plane,
};

const char* GetScenePrimitiveDisplayName(ScenePrimitive primitive);
bool TryParseScenePrimitive(const char* name, ScenePrimitive& outPrimitive);
const char* ScenePrimitiveToString(ScenePrimitive primitive);
