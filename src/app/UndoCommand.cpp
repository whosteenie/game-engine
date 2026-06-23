#include "app/UndoCommand.h"

#include "app/Scene.h"
#include "app/SceneDocument.h"
#include "app/SceneProjectIODetail.h"
#include "app/SceneSubtreeArchive.h"
#include "engine/Mesh.h"
#include "app/UndoStack.h"
#include "engine/LightComponent.h"
#include "engine/Material.h"
#include "engine/SceneObject.h"
#include "engine/SceneObjectId.h"

#include "engine/SceneHierarchy.h"

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
    context.scene.RestoreDeleteArchive(m_archive);
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
