#pragma once

#include "app/UndoContext.h"

#include "app/SceneDocument.h"

#include "engine/SceneObjectId.h"

#include <functional>
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
    SetObjectNameCommand(SceneObjectId objectId, std::string oldName, std::string newName);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplyName(UndoContext& context, const std::string& name) const;

    SceneObjectId m_objectId = kInvalidSceneObjectId;
    std::string m_oldName;
    std::string m_newName;
    std::string m_description;
};

void PushSetObjectName(
    class UndoStack& undoStack,
    Scene& scene,
    SceneObjectId objectId,
    const std::string& oldName,
    const std::string& newName);

class ApplySceneDocumentCommand final : public IUndoCommand
{
public:
    ApplySceneDocumentCommand(
        SceneDocument before,
        SceneDocument after,
        std::string name,
        std::string projectRoot);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;

private:
    void ApplySnapshot(UndoContext& context, const SceneDocument& document) const;

    SceneDocument m_before;
    SceneDocument m_after;
    std::string m_name;
    std::string m_projectRoot;
};

void PushSceneEdit(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& projectRoot,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);
