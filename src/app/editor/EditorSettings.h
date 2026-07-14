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
    bool IsVsyncEnabled() const { return m_vsyncEnabled; }
    void SetVsyncEnabled(bool enabled) { m_vsyncEnabled = enabled; }

    static std::string GetSettingsFilePath();
    static std::string GetGlobalImGuiIniPath();
    static std::string GetProjectImGuiIniPath(const std::string& projectRoot);
    static void EnsureAppDataDirectoryExists();
    static bool SaveGlobalEditorLayout();
    static bool SaveProjectEditorLayout(const std::string& projectRoot);
    static bool SaveEditorLayout(const std::string& projectRoot = {});
    static bool LoadGlobalEditorLayout();
    static bool LoadProjectEditorLayout(const std::string& projectRoot);
    static bool LoadEditorLayout(const std::string& projectRoot = {});
    static bool TryMigrateProjectEditorLayout(const std::string& projectRoot);
    static bool DeleteGlobalImGuiIni();
    static std::string NormalizeProjectFilePath(const std::string& projectFilePath);

private:
    std::vector<std::string> m_recentProjects;
    std::string m_lastNewProjectParentDirectory;
    bool m_vsyncEnabled = true;
};
