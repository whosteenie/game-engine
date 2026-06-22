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

    static std::string GetSettingsFilePath();

private:
    std::vector<std::string> m_recentProjects;
};
