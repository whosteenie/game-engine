#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "engine/SceneObjectId.h"

class ProjectSession;
class Scene;
class UndoStack;

class SceneHierarchyPanel
{
public:
    void Draw(Scene& scene, ProjectSession& project, UndoStack& undoStack) const;

    bool& ShowPanel() const { return m_showPanel; }

    const std::unordered_map<SceneObjectId, bool>& GetNodeOpenStates() const { return m_nodeOpenStates; }
    void SetNodeOpenStates(const std::unordered_map<SceneObjectId, bool>& openStates) { m_nodeOpenStates = openStates; }

    void PushSceneMutation(
        Scene& scene,
        const std::string& commandName,
        const std::function<void(Scene&)>& mutate) const;

private:
    mutable bool m_showPanel = true;
    mutable int m_pendingRenameIndex = -1;
    mutable int m_renameTargetIndex = -1;
    mutable bool m_beginRenameNextFrame = false;
    mutable char m_renameBuffer[128] = {};
    mutable bool m_focusRenameInput = false;
    mutable bool m_renameInputEngaged = false;
    mutable std::unordered_map<SceneObjectId, bool> m_nodeOpenStates;
    mutable bool m_scrollSelectionIntoView = false;
    mutable UndoStack* m_drawUndoStack = nullptr;
    mutable const std::string* m_drawProjectRoot = nullptr;
};
