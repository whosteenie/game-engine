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

            scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetTransform() = transform;
        }

        if (!transforms.empty())
        {
            scene.MarkDirty();
        }
    }
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

