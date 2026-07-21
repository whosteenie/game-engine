#pragma once

#include "engine/scene/InspectorComponentOrder.h"

#include <vector>

class Scene;
class SceneObject;

bool SceneObjectHasSystemComponent(const SceneObject& object, SceneSystemComponentType type);
bool CanAddSceneSystemComponent(const SceneObject& object, SceneSystemComponentType type);

void GetAddableSceneSystemComponents(const SceneObject& object, std::vector<SceneSystemComponentType>& out);

void AddSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type);
void RemoveSceneSystemComponent(Scene& scene, int objectIndex, SceneSystemComponentType type);
