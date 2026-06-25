#pragma once

#include <string>
#include <vector>

class EditorSettings
{
public:
    static constexpr int MaxRecentProjects = 10;

    void Load();
    void Save() const;

    const std::vector<std::string>& GetRecentProjects() const { return m_recentProjects; }
    void AddRecentProject(const std::string& projectFilePath);
    void RemoveRecentProject(const std::string& projectFilePath);
    bool PruneRecentProjects();

    const std::string& GetLastNewProjectParentDirectory() const { return m_lastNewProjectParentDirectory; }
    void SetLastNewProjectParentDirectory(const std::string& directory);
    void SetLastNewProjectParentDirectoryFromProjectFile(const std::string& projectFilePath);
    void ValidateLastNewProjectParentDirectory();

    static std::string GetSettingsFilePath();
    static std::string GetGlobalImGuiIniPath();
    static void EnsureAppDataDirectoryExists();
    static bool SaveGlobalEditorLayout();
    static bool LoadGlobalEditorLayout();
    static bool TryMigrateProjectEditorLayout(const std::string& projectRoot);
    static bool DeleteGlobalImGuiIni();
    static std::string NormalizeProjectFilePath(const std::string& projectFilePath);

private:
    std::vector<std::string> m_recentProjects;
    std::string m_lastNewProjectParentDirectory;
};
