#pragma once

#include "engine/scene/InspectorComponentOrder.h"

#include <vector>

class Scene;
class SceneObject;

enum class SceneSystemComponentType
{
    Light = 0,
    Camera,
    RigidBody,
    Collider,
};

const char* GetSceneSystemComponentLabel(SceneSystemComponentType type);

InspectorComponentType InspectorComponentTypeFromSystem(SceneSystemComponentType type);

bool SceneObjectHasSystemComponent(const SceneObject& object, SceneSystemComponentType type);
bool CanAddSceneSystemComponent(const SceneObject& object, SceneSystemComponentType type);

void GetAddableSceneSystemComponents(const SceneObject& object, std::vector<SceneSystemComponentType>& out);

void AddSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type);
void RemoveSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type);
