#pragma once

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

const char* GetInspectorComponentLabel(InspectorComponentType type);

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
