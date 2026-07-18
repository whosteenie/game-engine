#include "app/undo/UndoCommand.h"

#include "app/editor/EditorClipboard.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/document/Scene.h"
#include "app/project/SceneDocument.h"
#include "app/project/SceneProjectIODetail.h"
#include "app/project/SceneSubtreeArchive.h"
#include "engine/rendering/resources/Mesh.h"
#include "app/undo/UndoStack.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/resources/Material.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"

#include "engine/scene/SceneHierarchy.h"

bool AreObjectSystemComponentStatesEqual(
    const ObjectSystemComponentState& left,
    const ObjectSystemComponentState& right)
{
    return left.light == right.light
        && left.camera == right.camera
        && left.rigidBody == right.rigidBody
        && left.collider == right.collider
        && AreInspectorComponentOrdersEqual(left.inspectorOrder, right.inspectorOrder);
}

ObjectSystemComponentState CaptureObjectSystemComponentState(const SceneObject& object)
{
    ObjectSystemComponentState state;
    if (object.HasLight())
    {
        state.light = object.GetLight();
    }

    if (object.HasCamera())
    {
        state.camera = object.GetCamera();
    }

    if (object.HasRigidBody())
    {
        state.rigidBody = object.GetRigidBody();
    }

    if (object.HasCollider())
    {
        state.collider = object.GetCollider();
    }

    state.inspectorOrder = object.GetInspectorComponentOrder();
    if (state.inspectorOrder.empty())
    {
        state.inspectorOrder = object.GetEffectiveInspectorComponentOrder();
    }

    return state;
}

void ApplyObjectSystemComponentState(
    Scene& scene,
    SceneObjectId objectId,
    const ObjectSystemComponentState& state)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));

    if (state.light.has_value())
    {
        object.SetLight(*state.light);
    }
    else
    {
        object.ClearLight();
    }

    if (state.camera.has_value())
    {
        object.SetCamera(*state.camera);
        if (state.camera->isMain)
        {
            scene.EnsureUniqueMainCamera(objectIndex);
        }
    }
    else
    {
        object.ClearCamera();
    }

    if (state.rigidBody.has_value())
    {
        object.SetRigidBody(*state.rigidBody);
    }
    else
    {
        object.ClearRigidBody();
    }

    if (state.collider.has_value())
    {
        object.SetCollider(*state.collider);
    }
    else
    {
        object.ClearCollider();
    }

    object.SetInspectorComponentOrder(state.inspectorOrder);

    scene.MarkDirty();
}

class SetObjectSystemComponentStateCommand final : public IUndoCommand
{
public:
    SetObjectSystemComponentStateCommand(
        SceneObjectId objectId,
        ObjectSystemComponentState before,
        ObjectSystemComponentState after,
        std::string name)
        : m_objectId(objectId),
          m_before(std::move(before)),
          m_after(std::move(after)),
          m_name(std::move(name))
    {
    }

    void Undo(UndoContext& context) override
    {
        ApplyObjectSystemComponentState(context.scene, m_objectId, m_before);
    }

    void Redo(UndoContext& context) override
    {
        ApplyObjectSystemComponentState(context.scene, m_objectId, m_after);
    }

    const char* GetName() const override
    {
        return m_name.c_str();
    }

private:
    SceneObjectId m_objectId = kInvalidSceneObjectId;
    ObjectSystemComponentState m_before;
    ObjectSystemComponentState m_after;
    std::string m_name;
};

void PushSystemComponentMutation(
    UndoStack& undoStack,
    Scene& scene,
    int objectIndex,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate || objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjects().size()))
    {
        return;
    }

    const SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    const ObjectSystemComponentState before = CaptureObjectSystemComponentState(object);
    mutate(scene);
    const SceneObject& updatedObject = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    const ObjectSystemComponentState after = CaptureObjectSystemComponentState(updatedObject);
    if (AreObjectSystemComponentStatesEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<SetObjectSystemComponentStateCommand>(
        object.GetId(),
        before,
        after,
        commandName));
}

class SetInspectorComponentOrderCommand final : public IUndoCommand
{
public:
    SetInspectorComponentOrderCommand(
        SceneObjectId objectId,
        std::vector<InspectorComponentType> before,
        std::vector<InspectorComponentType> after,
        std::string name)
        : m_objectId(objectId),
          m_before(std::move(before)),
          m_after(std::move(after)),
          m_name(std::move(name))
    {
    }

    void Undo(UndoContext& context) override
    {
        ApplyOrder(context);
    }

    void Redo(UndoContext& context) override
    {
        ApplyOrder(context, true);
    }

    const char* GetName() const override
    {
        return m_name.c_str();
    }

private:
    void ApplyOrder(UndoContext& context, const bool useAfter = false)
    {
        const int objectIndex = context.scene.FindObjectIndex(m_objectId);
        if (objectIndex < 0)
        {
            return;
        }

        SceneObject& object = context.scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        object.SetInspectorComponentOrder(useAfter ? m_after : m_before);
        context.scene.MarkDirty();
    }

    SceneObjectId m_objectId = kInvalidSceneObjectId;
    std::vector<InspectorComponentType> m_before;
    std::vector<InspectorComponentType> m_after;
    std::string m_name;
};

void PushInspectorComponentOrderMutation(
    UndoStack& undoStack,
    Scene& scene,
    const int objectIndex,
    const std::string& commandName,
    const std::function<void(std::vector<InspectorComponentType>&)>& mutateOrder)
{
    if (!mutateOrder || objectIndex < 0 || objectIndex >= static_cast<int>(scene.GetObjects().size()))
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    std::vector<InspectorComponentType> before = object.GetEffectiveInspectorComponentOrder();
    std::vector<InspectorComponentType> after = before;
    mutateOrder(after);
    NormalizeInspectorComponentOrder(after, object);
    if (AreInspectorComponentOrdersEqual(before, after))
    {
        return;
    }

    object.SetInspectorComponentOrder(after);
    scene.MarkDirty();

    undoStack.Push(std::make_unique<SetInspectorComponentOrderCommand>(
        object.GetId(),
        std::move(before),
        std::move(after),
        commandName));
}

bool AreSceneEditorViewSettingsEqual(
    const SceneEditorViewSettings& left,
    const SceneEditorViewSettings& right)
{
    return left.showGrid == right.showGrid && left.showLightGizmos == right.showLightGizmos;
}

SceneEditorViewSettings CaptureSceneEditorViewSettings(const Scene& scene)
{
  return SceneEditorViewSettings{scene.GetShowGrid(), scene.GetShowLightGizmos()};
}

void ApplySceneEditorViewSettings(Scene& scene, const SceneEditorViewSettings& settings)
{
    scene.SetShowGrid(settings.showGrid);
    scene.SetShowLightGizmos(settings.showLightGizmos);
}

class SceneEditorViewSettingsCommand final : public IUndoCommand
{
public:
    SceneEditorViewSettingsCommand(
        SceneEditorViewSettings before,
        SceneEditorViewSettings after,
        std::string name)
        : m_before(before),
          m_after(std::move(after)),
          m_name(std::move(name))
    {
    }

    void Undo(UndoContext& context) override
    {
        ApplySceneEditorViewSettings(context.scene, m_before);
    }

    void Redo(UndoContext& context) override
    {
        ApplySceneEditorViewSettings(context.scene, m_after);
    }

    const char* GetName() const override
    {
        return m_name.c_str();
    }

    bool TryMerge(const IUndoCommand& next) override
    {
        const auto* other = dynamic_cast<const SceneEditorViewSettingsCommand*>(&next);
        if (other == nullptr)
        {
            return false;
        }

        m_after = other->m_after;
        return true;
    }

private:
    SceneEditorViewSettings m_before;
    SceneEditorViewSettings m_after;
    std::string m_name;
};

void PushSceneEditorViewMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    const SceneEditorViewSettings before = CaptureSceneEditorViewSettings(scene);
    mutate(scene);
    const SceneEditorViewSettings after = CaptureSceneEditorViewSettings(scene);
    if (AreSceneEditorViewSettingsEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<SceneEditorViewSettingsCommand>(before, after, commandName));
}

