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


namespace
{
    template<typename T, typename HasProperty, typename GetProperty>
    std::unordered_map<SceneObjectId, T> CaptureObjectProperties(
        const Scene& scene,
        const std::vector<int>& objectIndices,
        HasProperty hasProperty,
        GetProperty getProperty)
    {
        std::unordered_map<SceneObjectId, T> values;
        const std::vector<SceneObject>& objects = scene.GetObjects();

        for (int objectIndex : objectIndices)
        {
            if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
            {
                continue;
            }

            const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
            if (!hasProperty(object))
            {
                continue;
            }

            values.emplace(object.GetId(), getProperty(object));
        }

        return values;
    }

    template<typename T, typename HasProperty, typename GetProperty>
    std::unordered_map<SceneObjectId, T> CaptureAllObjectProperties(
        const Scene& scene,
        HasProperty hasProperty,
        GetProperty getProperty)
    {
        std::unordered_map<SceneObjectId, T> values;
        const std::vector<SceneObject>& objects = scene.GetObjects();
        values.reserve(objects.size());

        for (const SceneObject& object : objects)
        {
            if (!hasProperty(object))
            {
                continue;
            }

            values.emplace(object.GetId(), getProperty(object));
        }

        return values;
    }

    template<typename T>
    bool AreObjectPropertyMapsEqual(
        const std::unordered_map<SceneObjectId, T>& left,
        const std::unordered_map<SceneObjectId, T>& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (const auto& [objectId, value] : left)
        {
            const auto iterator = right.find(objectId);
            if (iterator == right.end() || !(value == iterator->second))
            {
                return false;
            }
        }

        return true;
    }
}

ObjectMaterialMap CaptureObjectMaterials(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectMaterialMap materials;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasMaterial())
        {
            continue;
        }

        materials.emplace(object.GetId(), object.GetMaterial().Clone());
    }

    return materials;
}

ObjectLightMap CaptureObjectLights(const Scene& scene, const std::vector<int>& objectIndices)
{
    return CaptureObjectProperties<LightComponent>(
        scene,
        objectIndices,
        [](const SceneObject& object) { return object.HasLight(); },
        [](const SceneObject& object) { return object.GetLight(); });
}

ObjectLightMap CaptureAllObjectLights(const Scene& scene)
{
    return CaptureAllObjectProperties<LightComponent>(
        scene,
        [](const SceneObject& object) { return object.HasLight(); },
        [](const SceneObject& object) { return object.GetLight(); });
}

ObjectCameraMap CaptureObjectCameras(const Scene& scene, const std::vector<int>& objectIndices)
{
    return CaptureObjectProperties<CameraComponent>(
        scene,
        objectIndices,
        [](const SceneObject& object) { return object.HasCamera(); },
        [](const SceneObject& object) { return object.GetCamera(); });
}

ObjectCameraMap CaptureAllObjectCameras(const Scene& scene)
{
    return CaptureAllObjectProperties<CameraComponent>(
        scene,
        [](const SceneObject& object) { return object.HasCamera(); },
        [](const SceneObject& object) { return object.GetCamera(); });
}

ObjectRigidBodyMap CaptureObjectRigidBodies(const Scene& scene, const std::vector<int>& objectIndices)
{
    return CaptureObjectProperties<RigidBodyComponent>(
        scene,
        objectIndices,
        [](const SceneObject& object) { return object.HasRigidBody(); },
        [](const SceneObject& object) { return object.GetRigidBody(); });
}

ObjectColliderMap CaptureObjectColliders(const Scene& scene, const std::vector<int>& objectIndices)
{
    return CaptureObjectProperties<ColliderComponent>(
        scene,
        objectIndices,
        [](const SceneObject& object) { return object.HasCollider(); },
        [](const SceneObject& object) { return object.GetCollider(); });
}

ObjectShadowFlagsMap CaptureObjectShadowFlags(const Scene& scene, const std::vector<int>& objectIndices)
{
    return CaptureObjectProperties<ObjectShadowFlags>(
        scene,
        objectIndices,
        [](const SceneObject& /*object*/) { return true; },
        [](const SceneObject& object) {
            return ObjectShadowFlags{object.CastsShadow(), object.ReceivesShadow()};
        });
}

bool AreObjectMaterialMapsEqual(const ObjectMaterialMap& left, const ObjectMaterialMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, material] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end() || material == nullptr || iterator->second == nullptr)
        {
            return false;
        }

        const Material& leftMaterial = *material;
        const Material& rightMaterial = *iterator->second;
        if (!leftMaterial.ContentEquals(rightMaterial))
        {
            return false;
        }
    }

    return true;
}

bool AreObjectLightMapsEqual(const ObjectLightMap& left, const ObjectLightMap& right)
{
    return AreObjectPropertyMapsEqual(left, right);
}

bool AreObjectCameraMapsEqual(const ObjectCameraMap& left, const ObjectCameraMap& right)
{
    return AreObjectPropertyMapsEqual(left, right);
}

bool AreObjectRigidBodyMapsEqual(const ObjectRigidBodyMap& left, const ObjectRigidBodyMap& right)
{
    return AreObjectPropertyMapsEqual(left, right);
}

bool AreObjectColliderMapsEqual(const ObjectColliderMap& left, const ObjectColliderMap& right)
{
    return AreObjectPropertyMapsEqual(left, right);
}

bool AreObjectShadowFlagsMapsEqual(const ObjectShadowFlagsMap& left, const ObjectShadowFlagsMap& right)
{
    return AreObjectPropertyMapsEqual(left, right);
}

void ApplyObjectMaterial(Scene& scene, SceneObjectId objectId, const std::unique_ptr<Material>& material)
{
    if (material == nullptr)
    {
        return;
    }

    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    if (!object.HasMaterial())
    {
        return;
    }

    object.ReplaceMaterial(material->Clone());
    scene.MarkDirty();
}

void ApplyObjectLight(Scene& scene, SceneObjectId objectId, const LightComponent& light)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    if (!object.HasLight())
    {
        return;
    }

    object.SetLight(light);
    scene.MarkDirty();
}

void ApplyObjectCamera(Scene& scene, SceneObjectId objectId, const CameraComponent& camera)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    if (!object.HasCamera())
    {
        return;
    }

    object.SetCamera(camera);
    if (camera.isMain)
    {
        scene.EnsureUniqueMainCamera(objectIndex);
    }

    scene.MarkDirty();
}

void ApplyObjectRigidBody(Scene& scene, SceneObjectId objectId, const RigidBodyComponent& rigidBody)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    if (!object.HasRigidBody())
    {
        return;
    }

    object.SetRigidBody(rigidBody);
    scene.MarkDirty();
}

void ApplyObjectCollider(Scene& scene, SceneObjectId objectId, const ColliderComponent& collider)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    if (!object.HasCollider())
    {
        return;
    }

    object.SetCollider(collider);
    scene.MarkDirty();
}

void ApplyObjectShadowFlags(Scene& scene, SceneObjectId objectId, const ObjectShadowFlags& flags)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    if (objectIndex < 0)
    {
        return;
    }

    SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
    object.SetCastShadow(flags.castShadow);
    object.SetReceiveShadow(flags.receiveShadow);
    scene.MarkDirty();
}

SetObjectMaterialsCommand::SetObjectMaterialsCommand(
    ObjectMaterialMap before,
    ObjectMaterialMap after,
    std::string name)
    : m_before(std::move(before)),
      m_after(std::move(after)),
      m_name(std::move(name))
{
}

void SetObjectMaterialsCommand::ApplyMaterials(
    UndoContext& context,
    const ObjectMaterialMap& materials) const
{
    for (const auto& [objectId, material] : materials)
    {
        ApplyObjectMaterial(context.scene, objectId, material);
    }
}

void SetObjectMaterialsCommand::Undo(UndoContext& context)
{
    ApplyMaterials(context, m_before);
}

void SetObjectMaterialsCommand::Redo(UndoContext& context)
{
    ApplyMaterials(context, m_after);
}

const char* SetObjectMaterialsCommand::GetName() const
{
    return m_name.c_str();
}

void PushObjectMaterials(
    UndoStack& undoStack,
    ObjectMaterialMap before,
    ObjectMaterialMap after,
    const std::string& commandName)
{
    if (before.empty() || AreObjectMaterialMapsEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<SetObjectMaterialsCommand>(
        std::move(before),
        std::move(after),
        commandName));
}

void PushMaterialMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate || objectIndices.empty())
    {
        return;
    }

    ObjectMaterialMap before = CaptureObjectMaterials(scene, objectIndices);
    mutate(scene);
    ObjectMaterialMap after = CaptureObjectMaterials(scene, objectIndices);
    PushObjectMaterials(undoStack, std::move(before), std::move(after), commandName);
}

void PushLightMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<ObjectLightMap(const Scene&, const std::vector<int>&)>& capture,
    const std::function<void(Scene&)>& mutate)
{
    PushPropertyMutation<LightComponent>(
        undoStack,
        scene,
        objectIndices,
        commandName,
        capture,
        AreObjectLightMapsEqual,
        ApplyObjectLight,
        mutate);
}

void PushShadowFlagsMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    PushPropertyMutation<ObjectShadowFlags>(
        undoStack,
        scene,
        objectIndices,
        commandName,
        [](const Scene& targetScene, const std::vector<int>& indices) {
            return CaptureObjectShadowFlags(targetScene, indices);
        },
        AreObjectShadowFlagsMapsEqual,
        ApplyObjectShadowFlags,
        mutate);
}

void BeginMaterialEditFrame(MaterialEditContext& context)
{
    if (context.scene == nullptr || context.objectIndices.empty())
    {
        context.hasFrameBefore = false;
        return;
    }
    context.frameBefore = CaptureObjectMaterials(*context.scene, context.objectIndices);
    context.hasFrameBefore = true;
}

void HandleMaterialFieldEditEvents(MaterialEditContext& context)
{
    if (context.undoStack == nullptr || context.scene == nullptr || context.objectIndices.empty())
    {
        return;
    }

    if (ImGui::IsItemActivated())
    {
        if (context.sessionOpen)
        {
            // Commit the previous widget's edit when focus moves to another material field without
            // a deactivate (common when clicking between sliders). ObjectMaterialMap is move-only
            // (unique_ptr<Material>), so "after" is a fresh capture; the current field's frame-start
            // snapshot below still gives that field's correct pre-edit value.
            ObjectMaterialMap after = CaptureObjectMaterials(*context.scene, context.objectIndices);
            if (!AreObjectMaterialMapsEqual(context.pendingBefore, after))
            {
                PushObjectMaterials(
                    *context.undoStack,
                    std::move(context.pendingBefore),
                    std::move(after),
                    context.commandName);
            }
            context.sessionOpen = false;
        }

        // Take "before" from the frame-start snapshot (moved — the map is move-only), NOT the
        // current state: clicking a slider track jumps the value on the same frame IsItemActivated
        // fires, so a live capture here would record the post-jump value and lose the click portion
        // on undo. Only one widget activates per frame, so consuming the snapshot is safe.
        if (context.hasFrameBefore)
        {
            context.pendingBefore = std::move(context.frameBefore);
            context.hasFrameBefore = false;
        }
        else
        {
            context.pendingBefore = CaptureObjectMaterials(*context.scene, context.objectIndices);
        }
        context.sessionOpen = true;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && context.sessionOpen)
    {
        ObjectMaterialMap after = CaptureObjectMaterials(*context.scene, context.objectIndices);
        PushObjectMaterials(
            *context.undoStack,
            std::move(context.pendingBefore),
            std::move(after),
            context.commandName);
        context.sessionOpen = false;
    }
}

void BeginLightFieldEditFrame(LightEditContext& context)
{
    BeginPropertyEditFrame<LightComponent>(
        context,
        [](const Scene& scene, const std::vector<int>& indices) {
            return CaptureObjectLights(scene, indices);
        });
}

void HandleLightFieldEditEvents(LightEditContext& context)
{
    HandlePropertyFieldEditEvents<LightComponent>(
        context,
        [](const Scene& scene, const std::vector<int>& indices) {
            return CaptureObjectLights(scene, indices);
        },
        AreObjectLightMapsEqual,
        ApplyObjectLight);
}

void PushCameraMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<ObjectCameraMap(const Scene&, const std::vector<int>&)>& capture,
    const std::function<void(Scene&)>& mutate)
{
    PushPropertyMutation<CameraComponent>(
        undoStack,
        scene,
        objectIndices,
        commandName,
        capture,
        AreObjectCameraMapsEqual,
        ApplyObjectCamera,
        mutate);
}

void PushRigidBodyMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    PushPropertyMutation<RigidBodyComponent>(
        undoStack,
        scene,
        objectIndices,
        commandName,
        CaptureObjectRigidBodies,
        AreObjectRigidBodyMapsEqual,
        ApplyObjectRigidBody,
        mutate);
}

void PushColliderMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    PushPropertyMutation<ColliderComponent>(
        undoStack,
        scene,
        objectIndices,
        commandName,
        CaptureObjectColliders,
        AreObjectColliderMapsEqual,
        ApplyObjectCollider,
        mutate);
}

void BeginCameraFieldEditFrame(CameraEditContext& context)
{
    BeginPropertyEditFrame<CameraComponent>(context, CaptureObjectCameras);
}

void HandleCameraFieldEditEvents(CameraEditContext& context)
{
    HandlePropertyFieldEditEvents<CameraComponent>(
        context,
        CaptureObjectCameras,
        AreObjectCameraMapsEqual,
        ApplyObjectCamera);
}

void BeginRigidBodyFieldEditFrame(RigidBodyEditContext& context)
{
    BeginPropertyEditFrame<RigidBodyComponent>(context, CaptureObjectRigidBodies);
}

void HandleRigidBodyFieldEditEvents(RigidBodyEditContext& context)
{
    HandlePropertyFieldEditEvents<RigidBodyComponent>(
        context,
        CaptureObjectRigidBodies,
        AreObjectRigidBodyMapsEqual,
        ApplyObjectRigidBody);
}

void BeginColliderFieldEditFrame(ColliderEditContext& context)
{
    BeginPropertyEditFrame<ColliderComponent>(context, CaptureObjectColliders);
}

void HandleColliderFieldEditEvents(ColliderEditContext& context)
{
    HandlePropertyFieldEditEvents<ColliderComponent>(
        context,
        CaptureObjectColliders,
        AreObjectColliderMapsEqual,
        ApplyObjectCollider);
}

