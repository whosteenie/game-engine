#pragma once

#include "app/undo/UndoContext.h"

#include <memory>
#include <vector>

class IUndoCommand;

class UndoStack
{
public:
    static constexpr std::size_t DefaultMaxDepth = 100;

    UndoStack() = default;
    ~UndoStack();
    UndoStack(UndoStack&&) noexcept;
    UndoStack& operator=(UndoStack&&) noexcept;
    UndoStack(const UndoStack&) = delete;
    UndoStack& operator=(const UndoStack&) = delete;

    void Push(std::unique_ptr<IUndoCommand> command);

    bool CanUndo() const;
    bool CanRedo() const;

    void Undo(UndoContext& context);
    void Redo(UndoContext& context);

    void Clear();

    const char* GetUndoName() const;
    const char* GetRedoName() const;

private:
    std::vector<std::unique_ptr<IUndoCommand>> m_undo;
    std::vector<std::unique_ptr<IUndoCommand>> m_redo;
};
