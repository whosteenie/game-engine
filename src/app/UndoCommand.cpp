#include "app/UndoCommand.h"

#include "app/Scene.h"
#include "app/SceneDocument.h"
#include "app/UndoStack.h"
#include "engine/SceneObject.h"
#include "engine/SceneObjectId.h"

namespace
{
    const std::string kEmptyProjectRoot;
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
