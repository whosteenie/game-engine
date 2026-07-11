#pragma once

#include <string>
#include <unordered_map>

class ProjectSession;
class Scene;
class UndoStack;

class ProjectFilesPanel
{
public:
    void Draw(Scene& scene, ProjectSession& project, UndoStack& undoStack);

    bool& ShowPanel() const { return m_showPanel; }

    void GetBrowseState(
        std::string& outBrowsedDirectory,
        std::string& outSelectedPath,
        std::unordered_map<std::string, bool>& outFolderOpenStates) const;

    void SetBrowseState(
        const std::string& browsedDirectory,
        const std::string& selectedPath,
        const std::unordered_map<std::string, bool>& folderOpenStates);

private:
    void ResetBrowseState(const std::string& projectRoot);
    void DrawToolbar(ProjectSession& project);
    void DrawFolderTree(ProjectSession& project, const std::string& directory);
    void DrawFileList(ProjectSession& project, const std::string& directory);
    void DrawEntryContextMenu(
        ProjectSession& project,
        const std::string& entryPath,
        const std::string& entryName,
        bool isDirectory);
    void DrawDeleteConfirmPopup();
    void BeginRename(const std::string& entryPath);
    void CancelRename();
    bool TryCommitRename();
    bool TryDeletePath(const std::string& entryPath);
    void HandleFilesPanelHotkeys();
    void ImportModelIntoScene(ProjectSession& project, const std::string& modelPath);

    mutable bool m_showPanel = true;
    mutable std::string m_browsedDirectory;
    mutable std::string m_selectedEntryPath;
    mutable std::string m_trackedProjectRoot;
    mutable std::unordered_map<std::string, bool> m_folderOpenStates;
    mutable bool m_scrollSelectionIntoView = false;

    mutable std::string m_renamePath;
    mutable char m_renameBuffer[260] = {};
    mutable bool m_beginRenameNextFrame = false;
    mutable bool m_focusRenameInput = false;
    mutable bool m_renameInputEngaged = false;

    mutable std::string m_pendingDeletePath;
    mutable bool m_openDeleteConfirmPopup = false;
    mutable std::string m_statusMessage;

    mutable Scene* m_drawScene = nullptr;
    mutable UndoStack* m_drawUndoStack = nullptr;
};
