#pragma once

class Scene;

class SceneHierarchyPanel
{
public:
    void Draw(Scene& scene) const;

private:
    mutable bool m_showPanel = true;
    mutable int m_pendingDeleteIndex = -1;
    mutable int m_pendingRenameIndex = -1;
    mutable int m_renameTargetIndex = -1;
    mutable bool m_beginRenameNextFrame = false;
    mutable char m_renameBuffer[128] = {};
    mutable bool m_focusRenameInput = false;
    mutable bool m_renameInputEngaged = false;
};
