#include "app/UndoCommand.h"

#include "app/Scene.h"
#include "app/SceneDocument.h"
#include "app/UndoStack.h"
#include "engine/SceneObject.h"
#include "engine/SceneObjectId.h"

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
