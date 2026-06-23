#pragma once

#include "app/UndoContext.h"

#include "app/SceneDocument.h"

#include "engine/LightComponent.h"
#include "engine/Material.h"
#include "engine/SceneObjectId.h"
#include "engine/Transform.h"

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class IUndoCommand
{
public:
    virtual ~IUndoCommand() = default;

    virtual void Undo(UndoContext& context) = 0;
    virtual void Redo(UndoContext& context) = 0;
    virtual const char* GetName() const = 0;

    virtual bool TryMerge(const IUndoCommand& next)
    {
        (void)next;
        return false;
    }
};

class SetObjectNameCommand final : public IUndoCommand
{
public:
    SetObjectNameCommand(SceneObjectId objectId, std::string oldName, std::string newName);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplyName(UndoContext& context, const std::string& name) const;

    SceneObjectId m_objectId = kInvalidSceneObjectId;
    std::string m_oldName;
    std::string m_newName;
    std::string m_description;
};

void PushSetObjectName(
    class UndoStack& undoStack,
    Scene& scene,
    SceneObjectId objectId,
    const std::string& oldName,
    const std::string& newName);

class ApplySceneDocumentCommand final : public IUndoCommand
{
public:
    ApplySceneDocumentCommand(
        SceneDocument before,
        SceneDocument after,
        std::string name,
        std::string projectRoot);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplySnapshot(UndoContext& context, const SceneDocument& document) const;

    SceneDocument m_before;
    SceneDocument m_after;
    std::string m_name;
    std::string m_projectRoot;
};

void PushSceneEdit(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& projectRoot,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

using ObjectTransformMap = std::unordered_map<SceneObjectId, Transform>;

ObjectTransformMap CaptureLocalTransforms(const Scene& scene, const std::vector<int>& objectIndices);
bool AreLocalTransformsEqual(const ObjectTransformMap& left, const ObjectTransformMap& right);

class TransformObjectsCommand final : public IUndoCommand
{
public:
    TransformObjectsCommand(
        ObjectTransformMap before,
        ObjectTransformMap after,
        std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;
    bool TryMerge(const IUndoCommand& next) override;

private:
    void ApplyTransforms(UndoContext& context, const ObjectTransformMap& transforms) const;

    ObjectTransformMap m_before;
    ObjectTransformMap m_after;
    std::string m_name;
};

void PushTransformObjects(
    UndoStack& undoStack,
    ObjectTransformMap before,
    ObjectTransformMap after,
    const std::string& commandName);

void PushTransformMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

struct TransformEditContext
{
    UndoStack* undoStack = nullptr;
    Scene* scene = nullptr;
    std::vector<int> objectIndices;
    std::string commandName = "Transform";

    ObjectTransformMap pendingBefore;
    bool sessionOpen = false;
};

void HandleTransformFieldEditEvents(TransformEditContext& context);

struct ObjectShadowFlags
{
    bool castShadow = true;
    bool receiveShadow = true;
};

using ObjectMaterialMap = std::unordered_map<SceneObjectId, std::unique_ptr<Material>>;
using ObjectLightMap = std::unordered_map<SceneObjectId, LightComponent>;
using ObjectShadowFlagsMap = std::unordered_map<SceneObjectId, ObjectShadowFlags>;

ObjectMaterialMap CaptureObjectMaterials(const Scene& scene, const std::vector<int>& objectIndices);
ObjectLightMap CaptureObjectLights(const Scene& scene, const std::vector<int>& objectIndices);
ObjectLightMap CaptureAllObjectLights(const Scene& scene);
ObjectShadowFlagsMap CaptureObjectShadowFlags(const Scene& scene, const std::vector<int>& objectIndices);

bool AreObjectMaterialMapsEqual(const ObjectMaterialMap& left, const ObjectMaterialMap& right);
bool AreObjectLightMapsEqual(const ObjectLightMap& left, const ObjectLightMap& right);
bool AreObjectShadowFlagsMapsEqual(const ObjectShadowFlagsMap& left, const ObjectShadowFlagsMap& right);

void ApplyObjectMaterial(Scene& scene, SceneObjectId objectId, const std::unique_ptr<Material>& material);
void ApplyObjectLight(Scene& scene, SceneObjectId objectId, const LightComponent& light);
void ApplyObjectShadowFlags(Scene& scene, SceneObjectId objectId, const ObjectShadowFlags& flags);

class SetObjectMaterialsCommand final : public IUndoCommand
{
public:
    SetObjectMaterialsCommand(ObjectMaterialMap before, ObjectMaterialMap after, std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplyMaterials(UndoContext& context, const ObjectMaterialMap& materials) const;

    ObjectMaterialMap m_before;
    ObjectMaterialMap m_after;
    std::string m_name;
};

void PushObjectMaterials(
    UndoStack& undoStack,
    ObjectMaterialMap before,
    ObjectMaterialMap after,
    const std::string& commandName);

template<typename T>
class ObjectPropertyCommand final : public IUndoCommand
{
public:
    using PropertyMap = std::unordered_map<SceneObjectId, T>;

    ObjectPropertyCommand(
        PropertyMap before,
        PropertyMap after,
        std::string name,
        std::function<void(Scene&, SceneObjectId, const T&)> applyProperty)
        : m_before(std::move(before)),
          m_after(std::move(after)),
          m_name(std::move(name)),
          m_applyProperty(std::move(applyProperty))
    {
    }

    void Undo(UndoContext& context) override
    {
        Apply(context, m_before);
    }

    void Redo(UndoContext& context) override
    {
        Apply(context, m_after);
    }

    const char* GetName() const override
    {
        return m_name.c_str();
    }

    bool TryMerge(const IUndoCommand& next) override
    {
        const auto* other = dynamic_cast<const ObjectPropertyCommand*>(&next);
        if (other == nullptr || !HasSameObjectIds(m_before, other->m_before))
        {
            return false;
        }

        if constexpr (std::is_copy_assignable_v<T>)
        {
            m_after = other->m_after;
            return true;
        }

        return false;
    }

private:
    static bool HasSameObjectIds(const PropertyMap& left, const PropertyMap& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (const auto& [objectId, value] : left)
        {
            (void)value;
            if (right.find(objectId) == right.end())
            {
                return false;
            }
        }

        return true;
    }

    void Apply(UndoContext& context, const PropertyMap& values) const
    {
        for (const auto& [objectId, value] : values)
        {
            m_applyProperty(context.scene, objectId, value);
        }
    }

    PropertyMap m_before;
    PropertyMap m_after;
    std::string m_name;
    std::function<void(Scene&, SceneObjectId, const T&)> m_applyProperty;
};

template<typename T>
void PushObjectProperty(
    UndoStack& undoStack,
    std::unordered_map<SceneObjectId, T> before,
    std::unordered_map<SceneObjectId, T> after,
    const std::string& commandName,
    const std::function<void(Scene&, SceneObjectId, const T&)>& applyProperty,
    const std::function<bool(const std::unordered_map<SceneObjectId, T>&, const std::unordered_map<SceneObjectId, T>&)>&
        equals)
{
    if (before.empty() || equals(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<ObjectPropertyCommand<T>>(
        std::move(before),
        std::move(after),
        commandName,
        applyProperty));
}

template<typename T>
struct PropertyEditContext
{
    UndoStack* undoStack = nullptr;
    Scene* scene = nullptr;
    std::vector<int> objectIndices;
    std::string commandName;

    std::unordered_map<SceneObjectId, T> pendingBefore;
    bool sessionOpen = false;
};

template<typename T>
void HandlePropertyFieldEditEvents(
    PropertyEditContext<T>& context,
    const std::function<std::unordered_map<SceneObjectId, T>(const Scene&, const std::vector<int>&)>& capture,
    const std::function<bool(const std::unordered_map<SceneObjectId, T>&, const std::unordered_map<SceneObjectId, T>&)>&
        equals,
    const std::function<void(Scene&, SceneObjectId, const T&)>& applyProperty)
{
    if (context.undoStack == nullptr || context.scene == nullptr || context.objectIndices.empty())
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = capture(*context.scene, context.objectIndices);
        context.sessionOpen = true;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && context.sessionOpen)
    {
        const std::unordered_map<SceneObjectId, T> after =
            capture(*context.scene, context.objectIndices);
        PushObjectProperty<T>(
            *context.undoStack,
            std::move(context.pendingBefore),
            after,
            context.commandName,
            applyProperty,
            equals);
        context.sessionOpen = false;
    }
}

template<typename T>
void PushPropertyMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<std::unordered_map<SceneObjectId, T>(const Scene&, const std::vector<int>&)>& capture,
    const std::function<bool(const std::unordered_map<SceneObjectId, T>&, const std::unordered_map<SceneObjectId, T>&)>&
        equals,
    const std::function<void(Scene&, SceneObjectId, const T&)>& applyProperty,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate || objectIndices.empty())
    {
        return;
    }

    const std::unordered_map<SceneObjectId, T> before = capture(scene, objectIndices);
    mutate(scene);
    const std::unordered_map<SceneObjectId, T> after = capture(scene, objectIndices);
    PushObjectProperty<T>(undoStack, before, after, commandName, applyProperty, equals);
}

struct MaterialEditContext
{
    UndoStack* undoStack = nullptr;
    Scene* scene = nullptr;
    std::vector<int> objectIndices;
    std::string commandName = "Material";

    ObjectMaterialMap pendingBefore;
    bool sessionOpen = false;
};

using LightEditContext = PropertyEditContext<LightComponent>;
using ShadowFlagsEditContext = PropertyEditContext<ObjectShadowFlags>;

void PushMaterialMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

void PushLightMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<std::unordered_map<SceneObjectId, LightComponent>(const Scene&, const std::vector<int>&)>&
        capture,
    const std::function<void(Scene&)>& mutate);

void PushShadowFlagsMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

void HandleMaterialFieldEditEvents(MaterialEditContext& context);
void HandleLightFieldEditEvents(LightEditContext& context);
