#include "app/UndoCommand.h"

#include "app/Scene.h"
#include "app/UndoStack.h"
#include "engine/SceneObject.h"

SetObjectNameCommand::SetObjectNameCommand(int objectIndex, std::string oldName, std::string newName)
    : m_objectIndex(objectIndex),
      m_oldName(std::move(oldName)),
      m_newName(std::move(newName))
{
    m_description = "Rename \"" + m_oldName + "\"";
}

void SetObjectNameCommand::ApplyName(UndoContext& context, const std::string& name) const
{
    if (m_objectIndex < 0 || static_cast<std::size_t>(m_objectIndex) >= context.scene.GetObjects().size())
    {
        return;
    }

    context.scene.GetObject(static_cast<std::size_t>(m_objectIndex)).SetName(name);
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
    int objectIndex,
    const std::string& oldName,
    const std::string& newName)
{
    if (objectIndex < 0 || oldName == newName)
    {
        return;
    }

    auto command = std::make_unique<SetObjectNameCommand>(objectIndex, oldName, newName);
    UndoContext context{scene};
    command->Redo(context);
    undoStack.Push(std::move(command));
}
