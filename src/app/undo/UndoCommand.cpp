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

#include <imgui.h>

#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace
{
    const std::string kEmptyProjectRoot;

    constexpr float kTransformEpsilon = 1e-4f;

    bool ApproximatelyEqual(float left, float right)
    {
        return std::fabs(left - right) <= kTransformEpsilon;
    }

    bool ApproximatelyEqual(const glm::vec3& left, const glm::vec3& right)
    {
        return ApproximatelyEqual(left.x, right.x)
            && ApproximatelyEqual(left.y, right.y)
            && ApproximatelyEqual(left.z, right.z);
    }

    bool ApproximatelyEqual(const glm::quat& left, const glm::quat& right)
    {
        const glm::quat normalizedLeft = glm::normalize(left);
        const glm::quat normalizedRight = glm::normalize(right);
        return std::fabs(glm::dot(normalizedLeft, normalizedRight)) >= 1.0f - kTransformEpsilon;
    }

    bool ApproximatelyEqual(const Transform& left, const Transform& right)
    {
        return ApproximatelyEqual(left.position, right.position)
            && ApproximatelyEqual(left.rotation, right.rotation)
            && ApproximatelyEqual(left.scale, right.scale);
    }

    bool HasSameObjectIds(const ObjectTransformMap& left, const ObjectTransformMap& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (const auto& [objectId, transform] : left)
        {
            (void)transform;
            if (right.find(objectId) == right.end())
            {
                return false;
            }
        }

        return true;
    }

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

    void ApplyLocalTransforms(Scene& scene, const ObjectTransformMap& transforms)
    {
        for (const auto& [objectId, transform] : transforms)
        {
            const int objectIndex = scene.FindObjectIndex(objectId);
            if (objectIndex < 0)
            {
                continue;
            }

            scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetTransform() = transform;
        }

        if (!transforms.empty())
        {
            scene.MarkDirty();
        }
    }
}

SetObjectNameCommand::SetObjectNameCommand(SceneObjectId objectId, std::string oldName, std::string newName)
    : m_objectId(objectId),
      m_oldName(std::move(oldName)),
      m_newName(std::move(newName))
{
    m_description = "Rename \"" + m_oldName + "\"";
}

void SetObjectNameCommand::ApplyName(UndoContext& context, const std::string& name) const
{
    const int objectIndex = context.scene.FindObjectIndex(m_objectId);
    if (objectIndex < 0)
    {
        return;
    }

    context.scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).SetName(name);
    context.scene.MarkDirty();
}

void SetObjectNameCommand::Undo(UndoContext& context)
{
    ApplyName(context, m_oldName);
}

void SetObjectNameCommand::Redo(UndoContext& context)
{
    ApplyName(context, m_newName);
}

const char* SetObjectNameCommand::GetName() const
{
    return m_description.c_str();
}

void PushSetObjectName(
    UndoStack& undoStack,
    Scene& scene,
    SceneObjectId objectId,
    const std::string& oldName,
    const std::string& newName)
{
    if (objectId == kInvalidSceneObjectId || oldName == newName)
    {
        return;
    }

    auto command = std::make_unique<SetObjectNameCommand>(objectId, oldName, newName);
    UndoContext context{scene, kEmptyProjectRoot};
    command->Redo(context);
    undoStack.Push(std::move(command));
}

ApplySceneDocumentCommand::ApplySceneDocumentCommand(
    SceneDocument before,
    SceneDocument after,
    std::string name,
    std::string projectRoot)
    : m_before(std::move(before)),
      m_after(std::move(after)),
      m_name(std::move(name)),
      m_projectRoot(std::move(projectRoot))
{
}

void ApplySceneDocumentCommand::ApplySnapshot(UndoContext& context, const SceneDocument& document) const
{
    std::string error;
    if (!document.Apply(context.scene, m_projectRoot, error, true))
    {
        return;
    }

    context.scene.MarkDirty();
}

void ApplySceneDocumentCommand::Undo(UndoContext& context)
{
    ApplySnapshot(context, m_before);
}

void ApplySceneDocumentCommand::Redo(UndoContext& context)
{
    ApplySnapshot(context, m_after);
}

const char* ApplySceneDocumentCommand::GetName() const
{
    return m_name.c_str();
}

void PushSceneEdit(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& projectRoot,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    SceneDocument before = SceneDocument::Capture(scene, projectRoot);
    mutate(scene);
    SceneDocument after = SceneDocument::Capture(scene, projectRoot);
    if (after.IsSameAs(before))
    {
        return;
    }

    undoStack.Push(std::make_unique<ApplySceneDocumentCommand>(
        std::move(before),
        std::move(after),
        commandName,
        projectRoot));
}

DeleteObjectsCommand::DeleteObjectsCommand(SceneSubtreeArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void DeleteObjectsCommand::Undo(UndoContext& context)
{
    context.scene.RestoreDeleteArchive(m_archive, m_archive.selectionBefore);
}

void DeleteObjectsCommand::Redo(UndoContext& context)
{
    const ArchivedSelectionState selectionBefore = m_archive.selectionBefore;

    std::vector<int> rootIndices;
    rootIndices.reserve(m_archive.removedRootIds.size());
    for (SceneObjectId rootId : m_archive.removedRootIds)
    {
        const int rootIndex = context.scene.FindObjectIndex(rootId);
        if (rootIndex >= 0)
        {
            rootIndices.push_back(rootIndex);
        }
    }

    if (rootIndices.empty() || !context.scene.CreateDeleteArchive(rootIndices, m_archive))
    {
        return;
    }

    m_archive.selectionBefore = selectionBefore;

    if (!context.scene.DeleteUsingArchive(m_archive))
    {
        return;
    }

    m_archive.selectionAfter = CaptureArchivedSelection(context.scene);
    ApplyArchivedSelection(context.scene, m_archive.selectionAfter);
    context.scene.MarkDirty();
}

const char* DeleteObjectsCommand::GetName() const
{
    return m_name.c_str();
}

void PushDeleteObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<int>& rootIndices)
{
    if (rootIndices.empty())
    {
        return;
    }

    SceneSubtreeArchive archive;
    archive.selectionBefore = CaptureArchivedSelection(scene);
    if (!scene.CreateDeleteArchive(rootIndices, archive))
    {
        return;
    }

    if (!scene.DeleteUsingArchive(archive))
    {
        return;
    }

    archive.selectionAfter = CaptureArchivedSelection(scene);

    undoStack.Push(std::make_unique<DeleteObjectsCommand>(std::move(archive), commandName));
}

void PushDeleteSelection(UndoStack& undoStack, Scene& scene, const std::string& commandName)
{
    const std::vector<int> roots =
        FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
    PushDeleteObjects(undoStack, scene, commandName, roots);
}

InsertSubtreeCommand::InsertSubtreeCommand(SceneSubtreeArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void InsertSubtreeCommand::Undo(UndoContext& context)
{
    std::vector<int> rootIndices;
    rootIndices.reserve(m_archive.removedRootIds.size());
    for (SceneObjectId rootId : m_archive.removedRootIds)
    {
        const int rootIndex = context.scene.FindObjectIndex(rootId);
        if (rootIndex >= 0)
        {
            rootIndices.push_back(rootIndex);
        }
    }

    // RestoreDeleteArchive moves component ownership into the live objects. Capture
    // them again before removing the inserted subtree so repeated undo/redo cycles
    // retain material and optional object components.
    if (rootIndices.empty() || !context.scene.CreateDeleteArchive(rootIndices, m_archive))
    {
        return;
    }

    if (!context.scene.DeleteUsingArchive(m_archive))
    {
        return;
    }

    ApplyArchivedSelection(context.scene, m_archive.selectionBefore);
    context.scene.MarkDirty();
}

void InsertSubtreeCommand::Redo(UndoContext& context)
{
    context.scene.RestoreDeleteArchive(m_archive, m_archive.selectionAfter);
}

const char* InsertSubtreeCommand::GetName() const
{
    return m_name.c_str();
}

ReparentObjectsCommand::ReparentObjectsCommand(ReparentArchive archive, std::string name)
    : m_archive(std::move(archive)),
      m_name(std::move(name))
{
}

void ReparentObjectsCommand::Undo(UndoContext& context)
{
    ApplyHierarchyArchive(context.scene, m_archive.before);
    ApplyArchivedSelection(context.scene, m_archive.selectionBefore);
}

void ReparentObjectsCommand::Redo(UndoContext& context)
{
    ApplyHierarchyArchive(context.scene, m_archive.after);
    ApplyArchivedSelection(context.scene, m_archive.selectionAfter);
}

const char* ReparentObjectsCommand::GetName() const
{
    return m_name.c_str();
}

void PushInsertSubtree(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<std::vector<int>(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    const ArchivedSelectionState selectionBefore = CaptureArchivedSelection(scene);
    const std::vector<int> insertedRoots = mutate(scene);
    if (insertedRoots.empty())
    {
        return;
    }

    SceneSubtreeArchive archive;
    if (!scene.CreateDeleteArchive(insertedRoots, archive))
    {
        return;
    }

    archive.selectionBefore = selectionBefore;
    archive.selectionAfter = CaptureArchivedSelection(scene);
    undoStack.Push(std::make_unique<InsertSubtreeCommand>(std::move(archive), commandName));
}

bool CopySelection(EditorClipboard& clipboard, Scene& scene)
{
    if (!scene.HasSelection())
    {
        return false;
    }

    const std::vector<int> roots =
        FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
    if (roots.empty())
    {
        return false;
    }

    SceneSubtreeArchive archive;
    if (!scene.CreateDeleteArchive(roots, archive, false))
    {
        return false;
    }

    clipboard.SetSubtreeArchive(std::move(archive));
    return true;
}

void CutSelection(UndoStack& undoStack, EditorClipboard& clipboard, Scene& scene)
{
    if (!CopySelection(clipboard, scene))
    {
        return;
    }

    PushDeleteSelection(undoStack, scene, "Cut");
}

void PushPasteFromClipboard(
    UndoStack& undoStack,
    Scene& scene,
    const EditorClipboard& clipboard,
    int referenceIndex,
    HierarchyInsertMode rootPlacement)
{
    const SceneSubtreeArchive* sourceArchive = clipboard.GetSubtreeArchive();
    if (sourceArchive == nullptr)
    {
        return;
    }

    PushInsertSubtree(undoStack, scene, "Paste", [&](Scene& target) {
        SceneSubtreeArchive workingCopy = CloneSubtreeArchive(*sourceArchive);
        RemapSubtreeArchiveIds(target, workingCopy);
        std::vector<int> insertedRoots =
            target.InsertSubtreeArchive(workingCopy, referenceIndex, rootPlacement);
        if (!insertedRoots.empty())
        {
            target.SetSelection(insertedRoots, insertedRoots.back());
        }

        return insertedRoots;
    });
}

void PushReparentObjects(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::vector<SceneObjectId>& objectIds,
    SceneObjectId referenceId,
    HierarchyInsertMode mode)
{
    const int referenceIndex = scene.FindObjectIndex(referenceId);
    if (referenceIndex < 0 || objectIds.empty())
    {
        return;
    }

    ReparentArchive archive;
    archive.selectionBefore = CaptureArchivedSelection(scene);
    archive.before = CaptureHierarchyArchive(scene);

    bool anyPlaced = false;
    for (SceneObjectId objectId : objectIds)
    {
        if (objectId == referenceId)
        {
            continue;
        }

        const int objectIndex = scene.FindObjectIndex(objectId);
        const int currentReferenceIndex = scene.FindObjectIndex(referenceId);
        if (objectIndex < 0 || currentReferenceIndex < 0)
        {
            continue;
        }

        if (!scene.CanPlaceObjectInHierarchy(objectIndex, currentReferenceIndex, mode)
            || !scene.WouldPlaceObjectInHierarchyChange(objectIndex, currentReferenceIndex, mode))
        {
            continue;
        }

        if (scene.PlaceObjectInHierarchy(objectIndex, currentReferenceIndex, mode))
        {
            anyPlaced = true;
        }
    }

    if (!anyPlaced)
    {
        return;
    }

    archive.after = CaptureHierarchyArchive(scene);
    if (AreHierarchyArchivesEqual(archive.before, archive.after))
    {
        return;
    }

    // Keep multi-select after a package reparent; single-object matches prior SelectSingle.
    if (objectIds.size() > 1)
    {
        ApplyArchivedSelection(scene, archive.selectionBefore);
    }
    else
    {
        const int selectedIndex = scene.FindObjectIndex(objectIds.front());
        if (selectedIndex >= 0)
        {
            scene.SelectSingle(selectedIndex);
        }
    }
    archive.selectionAfter = CaptureArchivedSelection(scene);
    undoStack.Push(std::make_unique<ReparentObjectsCommand>(std::move(archive), commandName));
}

ObjectTransformMap CaptureLocalTransforms(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectTransformMap transforms;
    transforms.reserve(objectIndices.size());

    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        transforms.emplace(object.GetId(), object.GetTransform());
    }

    return transforms;
}

bool AreLocalTransformsEqual(const ObjectTransformMap& left, const ObjectTransformMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, transform] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end() || !ApproximatelyEqual(transform, iterator->second))
        {
            return false;
        }
    }

    return true;
}

TransformObjectsCommand::TransformObjectsCommand(
    ObjectTransformMap before,
    ObjectTransformMap after,
    std::string name)
    : m_before(std::move(before)),
      m_after(std::move(after)),
      m_name(std::move(name))
{
}

void TransformObjectsCommand::ApplyTransforms(
    UndoContext& context,
    const ObjectTransformMap& transforms) const
{
    ApplyLocalTransforms(context.scene, transforms);
}

void TransformObjectsCommand::Undo(UndoContext& context)
{
    ApplyTransforms(context, m_before);
}

void TransformObjectsCommand::Redo(UndoContext& context)
{
    ApplyTransforms(context, m_after);
}

const char* TransformObjectsCommand::GetName() const
{
    return m_name.c_str();
}

bool TransformObjectsCommand::TryMerge(const IUndoCommand& /*next*/)
{
    return false;
}

void PushTransformObjects(
    UndoStack& undoStack,
    ObjectTransformMap before,
    ObjectTransformMap after,
    const std::string& commandName)
{
    if (before.empty() || AreLocalTransformsEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<TransformObjectsCommand>(
        std::move(before),
        std::move(after),
        commandName));
}

void PushTransformMutation(
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

    ObjectTransformMap before = CaptureLocalTransforms(scene, objectIndices);
    mutate(scene);
    ObjectTransformMap after = CaptureLocalTransforms(scene, objectIndices);
    if (!AreLocalTransformsEqual(before, after))
    {
        scene.MarkDirty();
    }

    PushTransformObjects(undoStack, std::move(before), std::move(after), commandName);
}

void HandleTransformFieldEditEvents(TransformEditContext& context)
{
    if (context.undoStack == nullptr || context.scene == nullptr || context.objectIndices.empty())
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = CaptureLocalTransforms(*context.scene, context.objectIndices);
        context.sessionOpen = true;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && context.sessionOpen)
    {
        const ObjectTransformMap after =
            CaptureLocalTransforms(*context.scene, context.objectIndices);
        PushTransformObjects(
            *context.undoStack,
            std::move(context.pendingBefore),
            std::move(after),
            context.commandName);
        context.sessionOpen = false;
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

nlohmann::json CaptureRendererSettings(const Scene& scene)
{
    return SceneProjectIODetail::SerializeRenderer(scene);
}

namespace
{
    nlohmann::json BuildRendererSettingsDelta(
        const nlohmann::json& before,
        const nlohmann::json& after,
        const nlohmann::json& values)
    {
        if (before == after)
        {
            return nlohmann::json::object();
        }

        if (!before.is_object() || !after.is_object() || !values.is_object())
        {
            return values;
        }

        nlohmann::json delta = nlohmann::json::object();
        for (const auto& [key, afterValue] : after.items())
        {
            const nlohmann::json beforeValue =
                before.contains(key) ? before.at(key) : nlohmann::json();
            const nlohmann::json value =
                values.contains(key) ? values.at(key) : nlohmann::json();
            nlohmann::json childDelta = BuildRendererSettingsDelta(beforeValue, afterValue, value);
            if (!childDelta.is_object() || !childDelta.empty())
            {
                delta[key] = std::move(childDelta);
            }
        }

        for (const auto& [key, beforeValue] : before.items())
        {
            if (after.contains(key))
            {
                continue;
            }

            const nlohmann::json value =
                values.contains(key) ? values.at(key) : nlohmann::json();
            delta[key] = value;
        }

        return delta;
    }

    void ApplyRendererSettingsDelta(Scene& scene, const nlohmann::json& delta)
    {
        SceneProjectIODetail::ApplyRendererSettingsDelta(scene, delta);
    }
}

bool AreRendererSettingsEqual(const nlohmann::json& left, const nlohmann::json& right)
{
    return left == right;
}

void ApplyRendererSettings(Scene& scene, const nlohmann::json& settings)
{
    SceneProjectIODetail::ApplyRendererSettingsDelta(scene, settings);
}

RendererSettingsCommand::RendererSettingsCommand(
    nlohmann::json before,
    nlohmann::json after,
    std::string name)
    : m_before(BuildRendererSettingsDelta(before, after, before)),
      m_after(BuildRendererSettingsDelta(before, after, after)),
      m_name(std::move(name))
{
}

void RendererSettingsCommand::Undo(UndoContext& context)
{
    ApplyRendererSettingsDelta(context.scene, m_before);
}

void RendererSettingsCommand::Redo(UndoContext& context)
{
    ApplyRendererSettingsDelta(context.scene, m_after);
}

const char* RendererSettingsCommand::GetName() const
{
    return m_name.c_str();
}

bool RendererSettingsCommand::TryMerge(const IUndoCommand& next)
{
    (void)next;
    return false;
}

void PushRendererSettings(
    UndoStack& undoStack,
    nlohmann::json before,
    nlohmann::json after,
    const std::string& commandName)
{
    if (AreRendererSettingsEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<RendererSettingsCommand>(
        std::move(before),
        std::move(after),
        commandName));
}

void PushRendererMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    nlohmann::json before = CaptureRendererSettings(scene);
    mutate(scene);
    nlohmann::json after = CaptureRendererSettings(scene);
    PushRendererSettings(undoStack, std::move(before), std::move(after), commandName);
}

void BeginRendererEditFrame(RendererEditContext& context)
{
    if (context.scene == nullptr)
    {
        context.hasFrameBefore = false;
        context.frameBefore = nlohmann::json();
        return;
    }

    context.frameBefore = CaptureRendererSettings(*context.scene);
    context.hasFrameBefore = true;
}

void HandleRendererFieldEditEvents(RendererEditContext& context)
{
    TuningSectionState::MarkCurrentItemIfSearchTarget();

    if (!TuningSectionState::CurrentItemIsUndoable())
    {
        return;
    }

    if (context.undoStack == nullptr || context.scene == nullptr)
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = context.hasFrameBefore
            ? context.frameBefore
            : CaptureRendererSettings(*context.scene);
        context.sessionOpen = true;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && context.sessionOpen)
    {
        nlohmann::json after = CaptureRendererSettings(*context.scene);
        PushRendererSettings(
            *context.undoStack,
            std::move(context.pendingBefore),
            std::move(after),
            context.commandName);
        context.sessionOpen = false;
    }
}
