#pragma once

#include "app/UndoContext.h"

#include "app/SceneDocument.h"

#include "engine/SceneObjectId.h"
#include "engine/Transform.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

using ObjectTransformMap = std::unordered_map<SceneObjectId, Transform>;

ObjectTransformMap CaptureLocalTransforms(const Scene& scene, const std::vector<int>& objectIndices);
bool AreLocalTransformsEqual(const ObjectTransformMap& left, const ObjectTransformMap& right);

class TransformObjectsCommand final : public IUndoCommand
{
public:
    TransformObjectsCommand(
        ObjectTransformMap before,
        ObjectTransformMap after,
        std::string name);

    void Undo(UndoContext& context) override;
    void Redo(UndoContext& context) override;
    const char* GetName() const override;
    bool TryMerge(const IUndoCommand& next) override;

private:
    void ApplyTransforms(UndoContext& context, const ObjectTransformMap& transforms) const;

    ObjectTransformMap m_before;
    ObjectTransformMap m_after;
    std::string m_name;
};

void PushTransformObjects(
    UndoStack& undoStack,
    ObjectTransformMap before,
    ObjectTransformMap after,
    const std::string& commandName);

void PushTransformMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::vector<int>& objectIndices,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate);

struct TransformEditContext
{
    UndoStack* undoStack = nullptr;
    Scene* scene = nullptr;
    std::vector<int> objectIndices;
    std::string commandName = "Transform";

    ObjectTransformMap pendingBefore;
    bool sessionOpen = false;
};

void HandleTransformFieldEditEvents(TransformEditContext& context);
