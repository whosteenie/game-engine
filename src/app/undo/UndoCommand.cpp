#include "app/undo/UndoCommand.h"

#include "app/editor/EditorClipboard.h"
#include "app/scene/Scene.h"
#include "app/project/SceneDocument.h"
#include "app/project/SceneProjectIODetail.h"
#include "app/project/SceneSubtreeArchive.h"
#include "engine/rendering/Mesh.h"
#include "app/undo/UndoStack.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/Material.h"
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

    void ApplyLocalTransforms(Scene& scene, const ObjectTransformMap& transforms)
    {
        for (const auto& [objectId, transform] : transforms)
        {
            const int objectIndex = scene.FindObjectIndex(objectId);
            if (objectIndex < 0)
            {
                continue;
            }

            scene.GetObject(static_cast<std::size_t>(objectIndex)).GetTransform() = transform;
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

    context.scene.GetObject(static_cast<std::size_t>(objectIndex)).SetName(name);
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
    SceneObjectId objectId,
    SceneObjectId referenceId,
    HierarchyInsertMode mode)
{
    const int objectIndex = scene.FindObjectIndex(objectId);
    const int referenceIndex = scene.FindObjectIndex(referenceId);
    if (objectIndex < 0 || referenceIndex < 0)
    {
        return;
    }

    if (!scene.CanPlaceObjectInHierarchy(objectIndex, referenceIndex, mode)
        || !scene.WouldPlaceObjectInHierarchyChange(objectIndex, referenceIndex, mode))
    {
        return;
    }

    ReparentArchive archive;
    archive.selectionBefore = CaptureArchivedSelection(scene);
    archive.before = CaptureHierarchyArchive(scene);
    if (!scene.PlaceObjectInHierarchy(objectIndex, referenceIndex, mode))
    {
        return;
    }

    archive.after = CaptureHierarchyArchive(scene);
    if (AreHierarchyArchivesEqual(archive.before, archive.after))
    {
        return;
    }

    scene.SelectSingle(objectIndex);
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

bool TransformObjectsCommand::TryMerge(const IUndoCommand& next)
{
    const auto* other = dynamic_cast<const TransformObjectsCommand*>(&next);
    if (other == nullptr || !HasSameObjectIds(m_before, other->m_before))
    {
        return false;
    }

    m_after = other->m_after;
    return true;
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
    ObjectLightMap lights;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasLight())
        {
            continue;
        }

        lights.emplace(object.GetId(), object.GetLight());
    }

    return lights;
}

ObjectLightMap CaptureAllObjectLights(const Scene& scene)
{
    ObjectLightMap lights;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    lights.reserve(objects.size());

    for (const SceneObject& object : objects)
    {
        if (!object.HasLight())
        {
            continue;
        }

        lights.emplace(object.GetId(), object.GetLight());
    }

    return lights;
}

ObjectCameraMap CaptureObjectCameras(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectCameraMap cameras;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasCamera())
        {
            continue;
        }

        cameras.emplace(object.GetId(), object.GetCamera());
    }

    return cameras;
}

ObjectCameraMap CaptureAllObjectCameras(const Scene& scene)
{
    ObjectCameraMap cameras;
    for (const SceneObject& object : scene.GetObjects())
    {
        if (!object.HasCamera())
        {
            continue;
        }

        cameras.emplace(object.GetId(), object.GetCamera());
    }

    return cameras;
}

ObjectRigidBodyMap CaptureObjectRigidBodies(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectRigidBodyMap rigidBodies;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasRigidBody())
        {
            continue;
        }

        rigidBodies.emplace(object.GetId(), object.GetRigidBody());
    }

    return rigidBodies;
}

ObjectColliderMap CaptureObjectColliders(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectColliderMap colliders;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        if (!object.HasCollider())
        {
            continue;
        }

        colliders.emplace(object.GetId(), object.GetCollider());
    }

    return colliders;
}

ObjectShadowFlagsMap CaptureObjectShadowFlags(const Scene& scene, const std::vector<int>& objectIndices)
{
    ObjectShadowFlagsMap flags;
    const std::vector<SceneObject>& objects = scene.GetObjects();

    for (int objectIndex : objectIndices)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            continue;
        }

        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        flags.emplace(
            object.GetId(),
            ObjectShadowFlags{object.CastsShadow(), object.ReceivesShadow()});
    }

    return flags;
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
        if (leftMaterial.GetAlbedo() != rightMaterial.GetAlbedo()
            || std::fabs(leftMaterial.GetRoughness() - rightMaterial.GetRoughness()) > 1e-4f
            || std::fabs(leftMaterial.GetMetallic() - rightMaterial.GetMetallic()) > 1e-4f
            || leftMaterial.IsDoubleSided() != rightMaterial.IsDoubleSided()
            || leftMaterial.HasAlbedoMap() != rightMaterial.HasAlbedoMap()
            || leftMaterial.HasNormalMap() != rightMaterial.HasNormalMap()
            || leftMaterial.HasAoMap() != rightMaterial.HasAoMap()
            || leftMaterial.HasRoughnessMap() != rightMaterial.HasRoughnessMap()
            || leftMaterial.HasMetallicRoughnessMap() != rightMaterial.HasMetallicRoughnessMap()
            || leftMaterial.GetAlbedoMapPath() != rightMaterial.GetAlbedoMapPath()
            || leftMaterial.GetNormalMapPath() != rightMaterial.GetNormalMapPath()
            || leftMaterial.GetAoMapPath() != rightMaterial.GetAoMapPath()
            || leftMaterial.GetRoughnessMapPath() != rightMaterial.GetRoughnessMapPath()
            || leftMaterial.GetAlbedoTexCoordSet() != rightMaterial.GetAlbedoTexCoordSet()
            || leftMaterial.GetNormalTexCoordSet() != rightMaterial.GetNormalTexCoordSet()
            || leftMaterial.GetAoTexCoordSet() != rightMaterial.GetAoTexCoordSet()
            || leftMaterial.GetRoughnessTexCoordSet() != rightMaterial.GetRoughnessTexCoordSet())
        {
            return false;
        }
    }

    return true;
}

bool AreObjectLightMapsEqual(const ObjectLightMap& left, const ObjectLightMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, light] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end())
        {
            return false;
        }

        const LightComponent& other = iterator->second;
        if (light.type != other.type
            || light.color != other.color
            || std::fabs(light.intensity - other.intensity) > 1e-4f
            || std::fabs(light.constantAttenuation - other.constantAttenuation) > 1e-4f
            || std::fabs(light.linearAttenuation - other.linearAttenuation) > 1e-4f
            || std::fabs(light.quadraticAttenuation - other.quadraticAttenuation) > 1e-4f
            || std::fabs(light.range - other.range) > 1e-4f
            || std::fabs(light.innerCutoffDegrees - other.innerCutoffDegrees) > 1e-4f
            || std::fabs(light.outerCutoffDegrees - other.outerCutoffDegrees) > 1e-4f
            || light.castsShadow != other.castsShadow)
        {
            return false;
        }
    }

    return true;
}

bool AreObjectCameraMapsEqual(const ObjectCameraMap& left, const ObjectCameraMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, camera] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end())
        {
            return false;
        }

        const CameraComponent& other = iterator->second;
        if (std::fabs(camera.fovDegrees - other.fovDegrees) > 1e-4f
            || std::fabs(camera.nearPlane - other.nearPlane) > 1e-4f
            || std::fabs(camera.farPlane - other.farPlane) > 1e-4f
            || camera.enabled != other.enabled
            || camera.depth != other.depth
            || camera.isMain != other.isMain)
        {
            return false;
        }
    }

    return true;
}

bool AreObjectRigidBodyMapsEqual(const ObjectRigidBodyMap& left, const ObjectRigidBodyMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, rigidBody] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end())
        {
            return false;
        }

        const RigidBodyComponent& other = iterator->second;
        if (std::fabs(rigidBody.mass - other.mass) > 1e-4f
            || rigidBody.useGravity != other.useGravity
            || rigidBody.isKinematic != other.isKinematic)
        {
            return false;
        }
    }

    return true;
}

bool AreObjectColliderMapsEqual(const ObjectColliderMap& left, const ObjectColliderMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, collider] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end())
        {
            return false;
        }

        const ColliderComponent& other = iterator->second;
        if (collider.shape != other.shape
            || collider.offset != other.offset
            || collider.halfExtents != other.halfExtents
            || std::fabs(collider.radius - other.radius) > 1e-4f
            || collider.isTrigger != other.isTrigger)
        {
            return false;
        }
    }

    return true;
}

bool AreObjectShadowFlagsMapsEqual(const ObjectShadowFlagsMap& left, const ObjectShadowFlagsMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (const auto& [objectId, flags] : left)
    {
        const auto iterator = right.find(objectId);
        if (iterator == right.end()
            || flags.castShadow != iterator->second.castShadow
            || flags.receiveShadow != iterator->second.receiveShadow)
        {
            return false;
        }
    }

    return true;
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

void HandleMaterialFieldEditEvents(MaterialEditContext& context)
{
    if (context.undoStack == nullptr || context.scene == nullptr || context.objectIndices.empty())
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = CaptureObjectMaterials(*context.scene, context.objectIndices);
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

void HandleCameraFieldEditEvents(CameraEditContext& context)
{
    HandlePropertyFieldEditEvents<CameraComponent>(
        context,
        CaptureObjectCameras,
        AreObjectCameraMapsEqual,
        ApplyObjectCamera);
}

void HandleRigidBodyFieldEditEvents(RigidBodyEditContext& context)
{
    HandlePropertyFieldEditEvents<RigidBodyComponent>(
        context,
        CaptureObjectRigidBodies,
        AreObjectRigidBodyMapsEqual,
        ApplyObjectRigidBody);
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
    if (left.light.has_value() != right.light.has_value())
    {
        return false;
    }

    if (left.light.has_value() && right.light.has_value())
    {
        const LightComponent& a = *left.light;
        const LightComponent& b = *right.light;
        if (a.type != b.type
            || a.color != b.color
            || std::fabs(a.intensity - b.intensity) > 1e-4f
            || std::fabs(a.constantAttenuation - b.constantAttenuation) > 1e-4f
            || std::fabs(a.linearAttenuation - b.linearAttenuation) > 1e-4f
            || std::fabs(a.quadraticAttenuation - b.quadraticAttenuation) > 1e-4f
            || std::fabs(a.range - b.range) > 1e-4f
            || std::fabs(a.innerCutoffDegrees - b.innerCutoffDegrees) > 1e-4f
            || std::fabs(a.outerCutoffDegrees - b.outerCutoffDegrees) > 1e-4f
            || a.castsShadow != b.castsShadow)
        {
            return false;
        }
    }

    if (left.camera.has_value() != right.camera.has_value())
    {
        return false;
    }

    if (left.camera.has_value() && right.camera.has_value())
    {
        const CameraComponent& a = *left.camera;
        const CameraComponent& b = *right.camera;
        if (std::fabs(a.fovDegrees - b.fovDegrees) > 1e-4f
            || std::fabs(a.nearPlane - b.nearPlane) > 1e-4f
            || std::fabs(a.farPlane - b.farPlane) > 1e-4f
            || a.enabled != b.enabled
            || a.depth != b.depth
            || a.isMain != b.isMain)
        {
            return false;
        }
    }

    if (left.rigidBody.has_value() != right.rigidBody.has_value())
    {
        return false;
    }

    if (left.rigidBody.has_value() && right.rigidBody.has_value())
    {
        const RigidBodyComponent& a = *left.rigidBody;
        const RigidBodyComponent& b = *right.rigidBody;
        if (std::fabs(a.mass - b.mass) > 1e-4f
            || a.useGravity != b.useGravity
            || a.isKinematic != b.isKinematic)
        {
            return false;
        }
    }

    if (left.collider.has_value() != right.collider.has_value())
    {
        return false;
    }

    if (left.collider.has_value() && right.collider.has_value())
    {
        const ColliderComponent& a = *left.collider;
        const ColliderComponent& b = *right.collider;
        if (a.shape != b.shape
            || a.offset != b.offset
            || a.halfExtents != b.halfExtents
            || std::fabs(a.radius - b.radius) > 1e-4f
            || a.isTrigger != b.isTrigger)
        {
            return false;
        }
    }

    return AreInspectorComponentOrdersEqual(left.inspectorOrder, right.inspectorOrder);
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));

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

    const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
    const ObjectSystemComponentState before = CaptureObjectSystemComponentState(object);
    mutate(scene);
    const SceneObject& updatedObject = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

        SceneObject& object = context.scene.GetObject(static_cast<std::size_t>(objectIndex));
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

    SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

bool AreRendererSettingsEqual(const nlohmann::json& left, const nlohmann::json& right)
{
    return left == right;
}

void ApplyRendererSettings(Scene& scene, const nlohmann::json& settings)
{
    SceneProjectIODetail::DeserializeRenderer(scene, settings);
    scene.MarkDirty();
}

RendererSettingsCommand::RendererSettingsCommand(
    nlohmann::json before,
    nlohmann::json after,
    std::string name)
    : m_before(std::move(before)),
      m_after(std::move(after)),
      m_name(std::move(name))
{
}

void RendererSettingsCommand::Undo(UndoContext& context)
{
    ApplyRendererSettings(context.scene, m_before);
}

void RendererSettingsCommand::Redo(UndoContext& context)
{
    ApplyRendererSettings(context.scene, m_after);
}

const char* RendererSettingsCommand::GetName() const
{
    return m_name.c_str();
}

bool RendererSettingsCommand::TryMerge(const IUndoCommand& next)
{
    const auto* other = dynamic_cast<const RendererSettingsCommand*>(&next);
    if (other == nullptr)
    {
        return false;
    }

    m_after = other->m_after;
    return true;
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

void HandleRendererFieldEditEvents(RendererEditContext& context)
{
    if (context.undoStack == nullptr || context.scene == nullptr)
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = CaptureRendererSettings(*context.scene);
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
