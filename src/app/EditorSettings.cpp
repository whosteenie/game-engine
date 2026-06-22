#include "app/EditorSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
    bool PathsEquivalent(const std::string& leftPath, const std::string& rightPath)
    {
        if (leftPath.empty() || rightPath.empty())
        {
            return false;
        }

        std::error_code error;
        return fs::equivalent(leftPath, rightPath, error);
    }
}

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

std::string EditorSettings::NormalizeProjectFilePath(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        return {};
    }

    std::error_code error;
    const fs::path normalizedPath = fs::weakly_canonical(fs::path(projectFilePath), error);
    return error ? projectFilePath : normalizedPath.string();
}

void EditorSettings::Load()
{
    m_recentProjects.clear();
    m_lastNewProjectParentDirectory.clear();

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
        m_recentProjects.clear();
        m_lastNewProjectParentDirectory.clear();
        Save();
        return;
    }

    if (root.contains("recentProjects") && root.at("recentProjects").is_array())
    {
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
    }

    if (root.contains("lastNewProjectParentDirectory") && root.at("lastNewProjectParentDirectory").is_string())
    {
        m_lastNewProjectParentDirectory = root.at("lastNewProjectParentDirectory").get<std::string>();
    }

    ValidateLastNewProjectParentDirectory();

    if (PruneRecentProjects())
    {
        Save();
    }
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
        if (projectPath.empty())
        {
            continue;
        }

        recentProjects.push_back(projectPath);
    }

    const json root = json{
        {"recentProjects", recentProjects},
        {"lastNewProjectParentDirectory", m_lastNewProjectParentDirectory},
    };

    std::ofstream output(settingsPath);
    if (!output.is_open())
    {
        return;
    }

    output << root.dump(2);
}

void EditorSettings::AddRecentProject(const std::string& projectFilePath)
{
    const std::string normalizedPath = NormalizeProjectFilePath(projectFilePath);
    if (normalizedPath.empty())
    {
        return;
    }

    std::error_code error;
    if (!fs::exists(normalizedPath, error) || !fs::is_regular_file(normalizedPath, error))
    {
        return;
    }

    m_recentProjects.erase(
        std::remove_if(
            m_recentProjects.begin(),
            m_recentProjects.end(),
            [&](const std::string& existingPath) {
                return PathsEquivalent(existingPath, normalizedPath) || existingPath == normalizedPath;
            }),
        m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), normalizedPath);

    if (static_cast<int>(m_recentProjects.size()) > MaxRecentProjects)
    {
        m_recentProjects.resize(MaxRecentProjects);
    }
}

void EditorSettings::RemoveRecentProject(const std::string& projectFilePath)
{
    const std::string normalizedPath = NormalizeProjectFilePath(projectFilePath);

    m_recentProjects.erase(
        std::remove_if(
            m_recentProjects.begin(),
            m_recentProjects.end(),
            [&](const std::string& existingPath) {
                if (existingPath.empty())
                {
                    return true;
                }

                if (!normalizedPath.empty()
                    && (PathsEquivalent(existingPath, normalizedPath) || existingPath == normalizedPath))
                {
                    return true;
                }

                return false;
            }),
        m_recentProjects.end());
}

bool EditorSettings::PruneRecentProjects()
{
    const std::vector<std::string> previousProjects = m_recentProjects;
    std::vector<std::string> cleanedProjects;
    cleanedProjects.reserve(previousProjects.size());

    for (const std::string& projectPath : previousProjects)
    {
        const std::string normalizedPath = NormalizeProjectFilePath(projectPath);
        if (normalizedPath.empty())
        {
            continue;
        }

        std::error_code error;
        if (!fs::exists(normalizedPath, error) || !fs::is_regular_file(normalizedPath, error))
        {
            continue;
        }

        const auto duplicateEntry = std::find_if(
            cleanedProjects.begin(),
            cleanedProjects.end(),
            [&](const std::string& existingPath) {
                return PathsEquivalent(existingPath, normalizedPath) || existingPath == normalizedPath;
            });
        if (duplicateEntry != cleanedProjects.end())
        {
            continue;
        }

        cleanedProjects.push_back(normalizedPath);
    }

    m_recentProjects = std::move(cleanedProjects);
    return m_recentProjects != previousProjects;
}

void EditorSettings::SetLastNewProjectParentDirectory(const std::string& directory)
{
    if (directory.empty())
    {
        m_lastNewProjectParentDirectory.clear();
        return;
    }

    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(fs::path(directory), error);
    m_lastNewProjectParentDirectory = error ? directory : canonical.string();
}

void EditorSettings::SetLastNewProjectParentDirectoryFromProjectFile(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        return;
    }

    std::error_code error;
    const fs::path projectRoot = fs::path(projectFilePath).parent_path();
    const fs::path browseDirectory = projectRoot.parent_path();
    if (!browseDirectory.empty() && fs::exists(browseDirectory, error) && fs::is_directory(browseDirectory, error))
    {
        SetLastNewProjectParentDirectory(browseDirectory.string());
        return;
    }

    SetLastNewProjectParentDirectory(projectRoot.string());
}

void EditorSettings::ValidateLastNewProjectParentDirectory()
{
    if (m_lastNewProjectParentDirectory.empty())
    {
        return;
    }

    std::error_code error;
    if (!fs::exists(m_lastNewProjectParentDirectory, error) || !fs::is_directory(m_lastNewProjectParentDirectory, error))
    {
        m_lastNewProjectParentDirectory.clear();
    }
}
