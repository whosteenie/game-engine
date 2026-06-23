#include "engine/scene/InspectorComponentOrder.h"

#include "engine/scene/SceneObject.h"

#include <algorithm>
#include <optional>

namespace
{
    struct InspectorComponentTypeEntry
    {
        InspectorComponentType type;
        const char* label;
        const char* jsonKey;
        std::optional<SceneSystemComponentType> systemType;
    };

    constexpr InspectorComponentTypeEntry kInspectorComponentRegistry[] = {
        {InspectorComponentType::Material, "Material", "material", std::nullopt},
        {InspectorComponentType::ObjectFlags, "Object", "objectFlags", std::nullopt},
        {InspectorComponentType::Light, "Light", "light", SceneSystemComponentType::Light},
        {InspectorComponentType::Camera, "Camera", "camera", SceneSystemComponentType::Camera},
        {InspectorComponentType::RigidBody, "Rigid Body", "rigidBody", SceneSystemComponentType::RigidBody},
        {InspectorComponentType::Collider, "Collider", "collider", SceneSystemComponentType::Collider},
    };

    const InspectorComponentTypeEntry* FindInspectorComponentEntry(const InspectorComponentType type)
    {
        for (const InspectorComponentTypeEntry& entry : kInspectorComponentRegistry)
        {
            if (entry.type == type)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    const InspectorComponentTypeEntry* FindInspectorComponentEntryByJsonKey(const std::string& jsonKey)
    {
        for (const InspectorComponentTypeEntry& entry : kInspectorComponentRegistry)
        {
            if (jsonKey == entry.jsonKey)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    const InspectorComponentTypeEntry* FindInspectorComponentEntryBySystemType(
        const SceneSystemComponentType systemType)
    {
        for (const InspectorComponentTypeEntry& entry : kInspectorComponentRegistry)
        {
            if (entry.systemType == systemType)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    bool ContainsInspectorComponentType(
        const std::vector<InspectorComponentType>& order,
        const InspectorComponentType type)
    {
        return std::find(order.begin(), order.end(), type) != order.end();
    }
}

const char* GetInspectorComponentLabel(const InspectorComponentType type)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntry(type);
    return entry != nullptr ? entry->label : "Unknown";
}

const char* GetInspectorComponentJsonKey(const InspectorComponentType type)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntry(type);
    return entry != nullptr ? entry->jsonKey : "unknown";
}

bool InspectorComponentTypeFromJsonKey(const std::string& value, InspectorComponentType& outType)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntryByJsonKey(value);
    if (entry == nullptr)
    {
        return false;
    }

    outType = entry->type;
    return true;
}

const char* GetSceneSystemComponentLabel(const SceneSystemComponentType type)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntryBySystemType(type);
    return entry != nullptr ? entry->label : "Unknown";
}

InspectorComponentType InspectorComponentTypeFromSystem(const SceneSystemComponentType type)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntryBySystemType(type);
    return entry != nullptr ? entry->type : InspectorComponentType::Light;
}

bool TryInspectorComponentTypeToSystemType(
    const InspectorComponentType type,
    SceneSystemComponentType& outType)
{
    const InspectorComponentTypeEntry* entry = FindInspectorComponentEntry(type);
    if (entry == nullptr || !entry->systemType.has_value())
    {
        return false;
    }

    outType = *entry->systemType;
    return true;
}

bool SceneObjectHasAnySystemComponent(const SceneObject& object)
{
    return object.HasLight()
        || object.HasCamera()
        || object.HasRigidBody()
        || object.HasCollider();
}

bool SceneObjectHasInspectorComponent(const SceneObject& object, const InspectorComponentType type)
{
    switch (type)
    {
    case InspectorComponentType::Material:
        return object.HasMaterial();
    case InspectorComponentType::ObjectFlags:
        return object.IsRenderable() && !SceneObjectHasAnySystemComponent(object);
    case InspectorComponentType::Light:
        return object.HasLight();
    case InspectorComponentType::Camera:
        return object.HasCamera();
    case InspectorComponentType::RigidBody:
        return object.HasRigidBody();
    case InspectorComponentType::Collider:
        return object.HasCollider();
    }

    return false;
}

std::vector<InspectorComponentType> BuildDefaultInspectorComponentOrder(const SceneObject& object)
{
    std::vector<InspectorComponentType> order;
    if (object.HasLight())
    {
        order.push_back(InspectorComponentType::Light);
    }

    if (SceneObjectHasInspectorComponent(object, InspectorComponentType::ObjectFlags))
    {
        order.push_back(InspectorComponentType::ObjectFlags);
    }

    if (object.HasMaterial())
    {
        order.push_back(InspectorComponentType::Material);
    }

    if (object.HasCamera())
    {
        order.push_back(InspectorComponentType::Camera);
    }

    if (object.HasRigidBody())
    {
        order.push_back(InspectorComponentType::RigidBody);
    }

    if (object.HasCollider())
    {
        order.push_back(InspectorComponentType::Collider);
    }

    return order;
}

void NormalizeInspectorComponentOrder(
    std::vector<InspectorComponentType>& order,
    const SceneObject& object)
{
    const std::vector<InspectorComponentType> defaultOrder = BuildDefaultInspectorComponentOrder(object);

    std::vector<InspectorComponentType> normalized;
    normalized.reserve(defaultOrder.size());

    for (const InspectorComponentType type : order)
    {
        if (!SceneObjectHasInspectorComponent(object, type)
            || ContainsInspectorComponentType(normalized, type))
        {
            continue;
        }

        normalized.push_back(type);
    }

    for (const InspectorComponentType type : defaultOrder)
    {
        if (!ContainsInspectorComponentType(normalized, type))
        {
            normalized.push_back(type);
        }
    }

    order = std::move(normalized);
}

void AppendInspectorComponentType(
    std::vector<InspectorComponentType>& order,
    const InspectorComponentType type)
{
    if (ContainsInspectorComponentType(order, type))
    {
        return;
    }

    order.push_back(type);
}

void RemoveInspectorComponentType(
    std::vector<InspectorComponentType>& order,
    const InspectorComponentType type)
{
    order.erase(
        std::remove(order.begin(), order.end(), type),
        order.end());
}

bool AreInspectorComponentOrdersEqual(
    const std::vector<InspectorComponentType>& left,
    const std::vector<InspectorComponentType>& right)
{
    return left == right;
}

bool MoveInspectorComponentOrderEntry(
    std::vector<InspectorComponentType>& order,
    const int fromIndex,
    const int toIndex)
{
    if (fromIndex < 0
        || toIndex < 0
        || fromIndex >= static_cast<int>(order.size())
        || toIndex >= static_cast<int>(order.size())
        || fromIndex == toIndex)
    {
        return false;
    }

    const InspectorComponentType moved = order[static_cast<std::size_t>(fromIndex)];
    order.erase(order.begin() + fromIndex);
    order.insert(order.begin() + toIndex, moved);
    return true;
}

bool WouldInspectorComponentMoveChangeOrder(
    const int fromIndex,
    const int beforeIndex,
    const int slotCount)
{
    if (fromIndex < 0 || beforeIndex < 0 || fromIndex >= slotCount || beforeIndex > slotCount)
    {
        return false;
    }

    int insertPos = beforeIndex;
    if (fromIndex < beforeIndex)
    {
        insertPos = beforeIndex - 1;
    }

    return insertPos != fromIndex;
}

bool MoveInspectorComponentBefore(
    std::vector<InspectorComponentType>& order,
    const int fromIndex,
    const int beforeIndex)
{
    if (fromIndex < 0 || beforeIndex < 0 || fromIndex >= static_cast<int>(order.size()))
    {
        return false;
    }

    const int slotCount = static_cast<int>(order.size());
    if (!WouldInspectorComponentMoveChangeOrder(fromIndex, beforeIndex, slotCount))
    {
        return false;
    }

    int insertPos = beforeIndex;
    if (fromIndex < beforeIndex)
    {
        insertPos = beforeIndex - 1;
    }

    insertPos = std::clamp(insertPos, 0, static_cast<int>(order.size()) - 1);

    const InspectorComponentType moved = order[static_cast<std::size_t>(fromIndex)];
    order.erase(order.begin() + fromIndex);
    order.insert(order.begin() + insertPos, moved);
    return true;
}
