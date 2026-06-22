#include "app/EditorSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string EditorSettings::GetSettingsFilePath()
{
    fs::path settingsDirectory;

#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"))
    {
        settingsDirectory = fs::path(appData) / "game-engine";
    }
    else
    {
        settingsDirectory = fs::path(".");
    }
#else
    if (const char* home = std::getenv("HOME"))
    {
        settingsDirectory = fs::path(home) / ".config" / "game-engine";
    }
    else
    {
        settingsDirectory = fs::path(".");
    }
#endif

    return (settingsDirectory / "editor_settings.json").string();
}

void EditorSettings::Load()
{
    m_recentProjects.clear();

    const std::string settingsPath = GetSettingsFilePath();
    std::ifstream input(settingsPath);
    if (!input.is_open())
    {
        return;
    }

    json root;
    try
    {
        input >> root;
    }
    catch (const json::exception&)
    {
        return;
    }

    if (!root.contains("recentProjects") || !root.at("recentProjects").is_array())
    {
        return;
    }

    for (const json& entry : root.at("recentProjects"))
    {
        if (!entry.is_string())
        {
            continue;
        }

        const std::string projectPath = entry.get<std::string>();
        if (!projectPath.empty())
        {
            m_recentProjects.push_back(projectPath);
        }
    }

    RemoveMissingRecentProjects();
}

void EditorSettings::Save() const
{
    const std::string settingsPath = GetSettingsFilePath();
    const fs::path settingsDirectory = fs::path(settingsPath).parent_path();

    std::error_code error;
    fs::create_directories(settingsDirectory, error);

    json recentProjects = json::array();
    for (const std::string& projectPath : m_recentProjects)
    {
        recentProjects.push_back(projectPath);
    }

    const json root = json{{"recentProjects", recentProjects}};

    std::ofstream output(settingsPath);
    if (!output.is_open())
    {
        return;
    }

    output << root.dump(2);
}

void EditorSettings::AddRecentProject(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        return;
    }

    m_recentProjects.erase(
        std::remove(m_recentProjects.begin(), m_recentProjects.end(), projectFilePath),
        m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), projectFilePath);

    if (static_cast<int>(m_recentProjects.size()) > MaxRecentProjects)
    {
        m_recentProjects.resize(MaxRecentProjects);
    }
}

void EditorSettings::RemoveMissingRecentProjects()
{
    m_recentProjects.erase(
        std::remove_if(
            m_recentProjects.begin(),
            m_recentProjects.end(),
            [](const std::string& projectPath) {
                std::error_code error;
                return !fs::exists(projectPath, error);
            }),
        m_recentProjects.end());
}
