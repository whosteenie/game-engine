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
