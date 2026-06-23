#include "app/undo/UndoStack.h"

void UndoStack::Push(std::unique_ptr<IUndoCommand> command)
{
    if (!command)
    {
        return;
    }

    m_redo.clear();

    if (!m_undo.empty() && m_undo.back()->TryMerge(*command))
    {
        return;
    }

    m_undo.push_back(std::move(command));
    if (m_undo.size() > DefaultMaxDepth)
    {
        m_undo.erase(m_undo.begin());
    }
}

bool UndoStack::CanUndo() const
{
    return !m_undo.empty();
}

bool UndoStack::CanRedo() const
{
    return !m_redo.empty();
}

void UndoStack::Undo(UndoContext& context)
{
    if (m_undo.empty())
    {
        return;
    }

    std::unique_ptr<IUndoCommand> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->Undo(context);
    m_redo.push_back(std::move(command));
}

void UndoStack::Redo(UndoContext& context)
{
    if (m_redo.empty())
    {
        return;
    }

    std::unique_ptr<IUndoCommand> command = std::move(m_redo.back());
    m_redo.pop_back();
    command->Redo(context);
    m_undo.push_back(std::move(command));
}

void UndoStack::Clear()
{
    m_undo.clear();
    m_redo.clear();
}

const char* UndoStack::GetUndoName() const
{
    if (m_undo.empty())
    {
        return "";
    }

    return m_undo.back()->GetName();
}

const char* UndoStack::GetRedoName() const
{
    if (m_redo.empty())
    {
        return "";
    }

    return m_redo.back()->GetName();
}
