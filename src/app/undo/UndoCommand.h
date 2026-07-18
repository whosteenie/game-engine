#pragma once

#include "app/undo/UndoContext.h"
#include "app/project/SceneSubtreeArchive.h"
#include "app/project/SceneDocument.h"

#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/resources/Material.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObjectId.h"
#include "engine/scene/Transform.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class UndoStack;

enum class HierarchyInsertMode;

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

class DeleteObjectsCommand final : public IUndoCommand
{
public:
    DeleteObjectsCommand(SceneSubtreeArchive archive, std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    SceneSubtreeArchive m_archive;
    std::string m_name;
};

class InsertSubtreeCommand final : public IUndoCommand
{
public:
    InsertSubtreeCommand(SceneSubtreeArchive archive, std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    SceneSubtreeArchive m_archive;
    std::string m_name;
};

class ReparentObjectsCommand final : public IUndoCommand
{
public:
    ReparentObjectsCommand(ReparentArchive archive, std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    ReparentArchive m_archive;
    std::string m_name;
};

void PushDeleteObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<int>& rootIndices);

void PushDeleteSelection(UndoStack& undoStack, Scene& scene, const std::string& commandName);

void PushInsertSubtree(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<std::vector<int>(Scene&)>& mutate);

class EditorClipboard;

bool CopySelection(EditorClipboard& clipboard, Scene& scene);
void CutSelection(UndoStack& undoStack, EditorClipboard& clipboard, Scene& scene);
void PushPasteFromClipboard(
    UndoStack& undoStack,
    Scene& scene,
    const EditorClipboard& clipboard,
    int referenceIndex,
    HierarchyInsertMode rootPlacement);

void PushReparentObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<SceneObjectId>& objectIds,
    SceneObjectId referenceId,
    HierarchyInsertMode mode);

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

inline bool operator==(const ObjectShadowFlags& left, const ObjectShadowFlags& right)
{
    return left.castShadow == right.castShadow && left.receiveShadow == right.receiveShadow;
}

using ObjectMaterialMap = std::unordered_map<SceneObjectId, std::unique_ptr<Material>>;
using ObjectLightMap = std::unordered_map<SceneObjectId, LightComponent>;
using ObjectCameraMap = std::unordered_map<SceneObjectId, CameraComponent>;
using ObjectRigidBodyMap = std::unordered_map<SceneObjectId, RigidBodyComponent>;
using ObjectColliderMap = std::unordered_map<SceneObjectId, ColliderComponent>;
using ObjectShadowFlagsMap = std::unordered_map<SceneObjectId, ObjectShadowFlags>;

struct ObjectSystemComponentState
{
    std::optional<LightComponent> light;
    std::optional<CameraComponent> camera;
    std::optional<RigidBodyComponent> rigidBody;
    std::optional<ColliderComponent> collider;
    std::vector<InspectorComponentType> inspectorOrder;
};

bool AreObjectSystemComponentStatesEqual(
    const ObjectSystemComponentState& left,
    const ObjectSystemComponentState& right);
ObjectSystemComponentState CaptureObjectSystemComponentState(const class SceneObject& object);
void ApplyObjectSystemComponentState(
    Scene& scene,
    SceneObjectId objectId,
    const ObjectSystemComponentState& state);

void PushSystemComponentMutation(
    UndoStack& undoStack,
    Scene& scene,
    int objectIndex,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

void PushInspectorComponentOrderMutation(
    UndoStack& undoStack,
    Scene& scene,
    int objectIndex,
    const std::string& commandName,
    const std::function<void(std::vector<InspectorComponentType>&)>& mutateOrder);

struct SceneEditorViewSettings
{
    bool showGrid = true;
    bool showLightGizmos = true;
};

bool AreSceneEditorViewSettingsEqual(
    const SceneEditorViewSettings& left,
    const SceneEditorViewSettings& right);
SceneEditorViewSettings CaptureSceneEditorViewSettings(const Scene& scene);
void ApplySceneEditorViewSettings(Scene& scene, const SceneEditorViewSettings& settings);

void PushSceneEditorViewMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

ObjectMaterialMap CaptureObjectMaterials(const Scene& scene, const std::vector<int>& objectIndices);
ObjectLightMap CaptureObjectLights(const Scene& scene, const std::vector<int>& objectIndices);
ObjectLightMap CaptureAllObjectLights(const Scene& scene);
ObjectCameraMap CaptureObjectCameras(const Scene& scene, const std::vector<int>& objectIndices);
ObjectCameraMap CaptureAllObjectCameras(const Scene& scene);
ObjectRigidBodyMap CaptureObjectRigidBodies(const Scene& scene, const std::vector<int>& objectIndices);
ObjectColliderMap CaptureObjectColliders(const Scene& scene, const std::vector<int>& objectIndices);
ObjectShadowFlagsMap CaptureObjectShadowFlags(const Scene& scene, const std::vector<int>& objectIndices);

bool AreObjectMaterialMapsEqual(const ObjectMaterialMap& left, const ObjectMaterialMap& right);
bool AreObjectLightMapsEqual(const ObjectLightMap& left, const ObjectLightMap& right);
bool AreObjectCameraMapsEqual(const ObjectCameraMap& left, const ObjectCameraMap& right);
bool AreObjectRigidBodyMapsEqual(const ObjectRigidBodyMap& left, const ObjectRigidBodyMap& right);
bool AreObjectColliderMapsEqual(const ObjectColliderMap& left, const ObjectColliderMap& right);
bool AreObjectShadowFlagsMapsEqual(const ObjectShadowFlagsMap& left, const ObjectShadowFlagsMap& right);

void ApplyObjectMaterial(Scene& scene, SceneObjectId objectId, const std::unique_ptr<Material>& material);
void ApplyObjectLight(Scene& scene, SceneObjectId objectId, const LightComponent& light);
void ApplyObjectCamera(Scene& scene, SceneObjectId objectId, const CameraComponent& camera);
void ApplyObjectRigidBody(Scene& scene, SceneObjectId objectId, const RigidBodyComponent& rigidBody);
void ApplyObjectCollider(Scene& scene, SceneObjectId objectId, const ColliderComponent& collider);
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

    bool TryMerge(const IUndoCommand& /*next*/) override
    {
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
    // Snapshot of the property BEFORE any widget ran this frame (set by BeginPropertyEditFrame).
    // A widget that begins a drag by clicking the track jumps its value AND reports IsItemActivated
    // on the same frame, so capturing "before" at activation captures the post-jump value — the
    // click portion is then lost on undo. Using the frame-start snapshot captures the true prior
    // value. Mirrors RendererEditContext::frameBefore.
    std::unordered_map<SceneObjectId, T> frameBefore;
    bool hasFrameBefore = false;
};

// Call once per frame BEFORE drawing this context's property widgets (see PropertyEditContext::
// frameBefore). objectIndices/scene must already be set for the frame.
template<typename T>
void BeginPropertyEditFrame(
    PropertyEditContext<T>& context,
    const std::function<std::unordered_map<SceneObjectId, T>(const Scene&, const std::vector<int>&)>& capture)
{
    if (context.scene == nullptr || context.objectIndices.empty())
    {
        context.hasFrameBefore = false;
        return;
    }
    context.frameBefore = capture(*context.scene, context.objectIndices);
    context.hasFrameBefore = true;
}

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
        context.pendingBefore = context.hasFrameBefore
            ? context.frameBefore
            : capture(*context.scene, context.objectIndices);
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
    // Snapshot of the material BEFORE any widget ran this frame (set by BeginMaterialEditFrame) —
    // see PropertyEditContext::frameBefore for the click-jump rationale this fixes.
    ObjectMaterialMap frameBefore;
    bool hasFrameBefore = false;
};

// Call once per frame BEFORE drawing the material widgets. objectIndices/scene must already be set.
void BeginMaterialEditFrame(MaterialEditContext& context);

using LightEditContext = PropertyEditContext<LightComponent>;
using CameraEditContext = PropertyEditContext<CameraComponent>;
using RigidBodyEditContext = PropertyEditContext<RigidBodyComponent>;
using ColliderEditContext = PropertyEditContext<ColliderComponent>;
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
    const std::function<ObjectLightMap(const Scene&, const std::vector<int>&)>& capture,
    const std::function<void(Scene&)>& mutate);

void PushCameraMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<ObjectCameraMap(const Scene&, const std::vector<int>&)>& capture,
    const std::function<void(Scene&)>& mutate);

void PushRigidBodyMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

void PushColliderMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

void PushShadowFlagsMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

// Begin*: call once per frame BEFORE drawing each context's widgets, to snapshot the pre-edit
// value (fixes slider click-jump losing the click portion on undo — see PropertyEditContext).
void BeginLightFieldEditFrame(LightEditContext& context);
void BeginCameraFieldEditFrame(CameraEditContext& context);
void BeginRigidBodyFieldEditFrame(RigidBodyEditContext& context);
void BeginColliderFieldEditFrame(ColliderEditContext& context);

void HandleMaterialFieldEditEvents(MaterialEditContext& context);
void HandleLightFieldEditEvents(LightEditContext& context);
void HandleCameraFieldEditEvents(CameraEditContext& context);
void HandleRigidBodyFieldEditEvents(RigidBodyEditContext& context);
void HandleColliderFieldEditEvents(ColliderEditContext& context);

nlohmann::json CaptureRendererSettings(const Scene& scene);
bool AreRendererSettingsEqual(const nlohmann::json& left, const nlohmann::json& right);
void ApplyRendererSettings(Scene& scene, const nlohmann::json& settings);

class RendererSettingsCommand final : public IUndoCommand
{
public:
    RendererSettingsCommand(nlohmann::json before, nlohmann::json after, std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;
    bool TryMerge(const IUndoCommand& next) override;

private:
    nlohmann::json m_before;
    nlohmann::json m_after;
    std::string m_name;
};

void PushRendererSettings(
    UndoStack& undoStack,
    nlohmann::json before,
    nlohmann::json after,
    const std::string& commandName);

void PushRendererMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

struct RendererEditContext
{
    UndoStack* undoStack = nullptr;
    Scene* scene = nullptr;
    std::string commandName = "Renderer";

    nlohmann::json frameBefore;
    nlohmann::json pendingBefore;
    bool hasFrameBefore = false;
    bool sessionOpen = false;
};

void BeginRendererEditFrame(RendererEditContext& context);
void HandleRendererFieldEditEvents(RendererEditContext& context);

/*
 * Adding a new undo command:
 * 1. Implement IUndoCommand with Undo/Redo/GetName (and TryMerge when edits should coalesce).
 * 2. Add Capture/Apply helpers if the command patches live scene state in place.
 * 3. Expose PushXxxSettings (before/after) and/or PushXxxMutation (capture ΓåÆ mutate ΓåÆ capture).
 * 4. Wire UI on edit end (IsItemDeactivatedAfterEdit) or transaction boundaries (gizmo drag).
 * 5. Use PushSceneEdit only for structural changes that need a full SceneDocument snapshot.
 */
