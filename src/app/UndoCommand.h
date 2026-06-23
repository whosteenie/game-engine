#pragma once

#include "app/UndoContext.h"

#include <memory>
#include <string>

class IUndoCommand
{
public:
    virtual ~IUndoCommand() = default;

    virtual void Undo(UndoContext& context) = 0;
    virtual void Redo(UndoContext& context) = 0;
    virtual const char* GetName() const = 0;

    virtual bool TryMerge(const IUndoCommand& next)
    {
        (void)next;
        return false;
    }
};

class SetObjectNameCommand final : public IUndoCommand
{
public:
    SetObjectNameCommand(int objectIndex, std::string oldName, std::string newName);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplyName(UndoContext& context, const std::string& name) const;

    int m_objectIndex = -1;
    std::string m_oldName;
    std::string m_newName;
    std::string m_description;
};

void PushSetObjectName(
    class UndoStack& undoStack,
    Scene& scene,
    int objectIndex,
    const std::string& oldName,
    const std::string& newName);
