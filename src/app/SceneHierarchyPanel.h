#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <vector>

#include "engine/SceneObjectId.h"

enum class HierarchyInsertMode;

class ProjectSession;
class Scene;
class UndoStack;
class EditorClipboard;

class SceneHierarchyPanel
{
public:
    void Draw(Scene& scene, ProjectSession& project, UndoStack& undoStack, EditorClipboard& clipboard) const;

    bool& ShowPanel() const { return m_showPanel; }

    const std::unordered_map<SceneObjectId, bool>& GetNodeOpenStates() const { return m_nodeOpenStates; }
    void SetNodeOpenStates(const std::unordered_map<SceneObjectId, bool>& openStates) { m_nodeOpenStates = openStates; }

    void PushInsertMutation(
        Scene& scene,
        const std::string& commandName,
        const std::function<std::vector<int>(Scene&)>& mutate) const;

    void PushReparentMutation(
        Scene& scene,
        const std::string& commandName,
        SceneObjectId objectId,
        SceneObjectId referenceId,
        HierarchyInsertMode mode) const;

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
    mutable EditorClipboard* m_drawClipboard = nullptr;
};
