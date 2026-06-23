#pragma once

#include <string>
#include <unordered_map>

class ProjectSession;

class ProjectFilesPanel
{
public:
    void Draw(ProjectSession& project);

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
    void DrawFolderTree(const std::string& directory);
    void DrawFileList(const std::string& directory);

    mutable bool m_showPanel = true;
    mutable std::string m_browsedDirectory;
    mutable std::string m_selectedEntryPath;
    mutable std::string m_trackedProjectRoot;
    mutable std::unordered_map<std::string, bool> m_folderOpenStates;
    mutable bool m_scrollSelectionIntoView = false;
};
