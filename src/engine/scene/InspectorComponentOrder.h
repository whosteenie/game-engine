#pragma once

#include <string>
#include <vector>

class SceneObject;

enum class InspectorComponentType
{
    Material = 0,
    ObjectFlags,
    Light,
    Camera,
    RigidBody,
    Collider,
};

enum class SceneSystemComponentType
{
    Light = 0,
    Camera,
    RigidBody,
    Collider,
};

const char* GetInspectorComponentLabel(InspectorComponentType type);
const char* GetInspectorComponentJsonKey(InspectorComponentType type);
bool InspectorComponentTypeFromJsonKey(const std::string& value, InspectorComponentType& outType);

const char* GetSceneSystemComponentLabel(SceneSystemComponentType type);
InspectorComponentType InspectorComponentTypeFromSystem(SceneSystemComponentType type);
bool TryInspectorComponentTypeToSystemType(InspectorComponentType type, SceneSystemComponentType& outType);

bool SceneObjectHasInspectorComponent(const SceneObject& object, InspectorComponentType type);

bool SceneObjectHasAnySystemComponent(const SceneObject& object);

std::vector<InspectorComponentType> BuildDefaultInspectorComponentOrder(const SceneObject& object);

void NormalizeInspectorComponentOrder(
    std::vector<InspectorComponentType>& order,
    const SceneObject& object);

void AppendInspectorComponentType(
    std::vector<InspectorComponentType>& order,
    InspectorComponentType type);

void RemoveInspectorComponentType(
    std::vector<InspectorComponentType>& order,
    InspectorComponentType type);

bool AreInspectorComponentOrdersEqual(
    const std::vector<InspectorComponentType>& left,
    const std::vector<InspectorComponentType>& right);

bool MoveInspectorComponentOrderEntry(
    std::vector<InspectorComponentType>& order,
    int fromIndex,
    int toIndex);

bool MoveInspectorComponentBefore(
    std::vector<InspectorComponentType>& order,
    int fromIndex,
    int beforeIndex);

bool WouldInspectorComponentMoveChangeOrder(
    int fromIndex,
    int beforeIndex,
    int slotCount);
