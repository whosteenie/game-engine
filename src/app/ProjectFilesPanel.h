#pragma once

#include <string>

class ProjectSession;

class ProjectFilesPanel
{
public:
    void Draw(ProjectSession& project);

    bool& ShowPanel() const { return m_showPanel; }

private:
    void ResetBrowseState(const std::string& projectRoot);
    void DrawToolbar(ProjectSession& project);
    void DrawFolderTree(const std::string& directory);
    void DrawFileList(const std::string& directory);

    mutable bool m_showPanel = true;
    mutable std::string m_browsedDirectory;
    mutable std::string m_selectedEntryPath;
    mutable std::string m_trackedProjectRoot;
};
