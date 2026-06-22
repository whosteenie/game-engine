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
    void RemoveMissingRecentProjects();

    const std::string& GetLastNewProjectParentDirectory() const { return m_lastNewProjectParentDirectory; }
    void SetLastNewProjectParentDirectory(const std::string& directory);
    void ValidateLastNewProjectParentDirectory();

    static std::string GetSettingsFilePath();

private:
    std::vector<std::string> m_recentProjects;
    std::string m_lastNewProjectParentDirectory;
};
