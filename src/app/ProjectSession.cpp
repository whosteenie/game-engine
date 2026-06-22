#include "app/ProjectSession.h"

#include "app/Scene.h"
#include "app/SceneProjectIO.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

void ProjectSession::MarkDirty()
{
    m_dirty = true;
}

void ProjectSession::SetStatusMessage(const std::string& message)
{
    m_statusMessage = message;
}

void ProjectSession::MarkClean()
{
    m_dirty = false;
}

void ProjectSession::CloseProject()
{
    m_projectFilePath.clear();
    m_projectRootDirectory.clear();
    m_displayName = "Untitled";
    m_statusMessage.clear();
    m_dirty = false;
    m_hasActiveProject = false;
}

std::string ProjectSession::SanitizeProjectName(const std::string& projectName)
{
    std::string sanitized;
    sanitized.reserve(projectName.size());

    bool previousWasSpace = false;
    for (char character : projectName)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (character == '\\' || character == '/' || character == ':' || character == '*' || character == '?'
            || character == '"' || character == '<' || character == '>' || character == '|')
        {
            continue;
        }

        if (std::isspace(value))
        {
            if (!sanitized.empty() && !previousWasSpace)
            {
                sanitized.push_back(' ');
                previousWasSpace = true;
            }
            continue;
        }

        previousWasSpace = false;
        sanitized.push_back(character);
    }

    while (!sanitized.empty() && sanitized.back() == ' ')
    {
        sanitized.pop_back();
    }

    if (sanitized.empty())
    {
        return "Project";
    }

    return sanitized;
}

bool ProjectSession::CreateNewProject(Scene& scene, const std::string& directory, const std::string& projectName)
{
    if (directory.empty())
    {
        m_statusMessage = "Choose a location for the new project folder.";
        return false;
    }

    const std::string sanitizedName = SanitizeProjectName(projectName);
    const fs::path projectDirectory = fs::path(directory) / sanitizedName;
    const fs::path projectPath = projectDirectory / (sanitizedName + ProjectFileExtension);

    std::error_code error;
    if (fs::exists(projectDirectory, error))
    {
        m_statusMessage = "A folder with that project name already exists in the selected location.";
        return false;
    }

    fs::create_directories(projectDirectory, error);
    if (error)
    {
        m_statusMessage = "Failed to create the project folder.";
        return false;
    }

    SetProjectRootDirectory(projectDirectory.string());
    SetProjectFilePath(projectPath.string());

    fs::create_directories(projectDirectory / "Assets", error);
    if (error)
    {
        m_statusMessage = "Failed to create the project Assets folder.";
        CloseProject();
        return false;
    }

    scene.ResetToDefault();

    if (!Save(scene))
    {
        CloseProject();
        return false;
    }

    m_hasActiveProject = true;
    m_statusMessage = "Created " + m_displayName;
    return true;
}

bool ProjectSession::Save(Scene& scene)
{
    if (m_projectFilePath.empty())
    {
        m_statusMessage = "Save requires a project file path. Use Save As.";
        return false;
    }

    std::string error;
    if (!SceneProjectIO::Save(scene, m_projectRootDirectory, m_projectFilePath, error))
    {
        m_statusMessage = error.empty() ? "Failed to save project." : error;
        return false;
    }

    MarkClean();
    m_statusMessage = "Saved " + m_displayName;
    return true;
}

bool ProjectSession::SaveAs(Scene& scene, const std::string& projectFilePath)
{
    SetProjectFilePath(projectFilePath);

    std::string error;
    if (!SceneProjectIO::Save(scene, m_projectRootDirectory, m_projectFilePath, error))
    {
        m_statusMessage = error.empty() ? "Failed to save project." : error;
        return false;
    }

    MarkClean();
    m_statusMessage = "Saved " + m_displayName;
    return true;
}

bool ProjectSession::OpenProject(Scene& scene, const std::string& projectFilePath)
{
    SetProjectFilePath(projectFilePath);

    std::string error;
    if (!SceneProjectIO::Load(scene, m_projectRootDirectory, m_projectFilePath, error))
    {
        m_statusMessage = error.empty() ? "Failed to load project." : error;
        m_hasActiveProject = false;
        return false;
    }

    MarkClean();
    m_hasActiveProject = true;
    m_statusMessage = "Opened " + m_displayName;
    return true;
}

void ProjectSession::SetProjectFilePath(const std::string& projectFilePath)
{
    m_projectFilePath = projectFilePath;
    m_displayName = ExtractDisplayName(projectFilePath);

    const fs::path parent = fs::path(projectFilePath).parent_path();
    if (!parent.empty())
    {
        SetProjectRootDirectory(parent.string());
    }
}

void ProjectSession::SetProjectRootDirectory(const std::string& directory)
{
    if (directory.empty())
    {
        m_projectRootDirectory.clear();
        return;
    }

    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(fs::path(directory), error);
    m_projectRootDirectory = error ? directory : canonical.string();
}

std::string ProjectSession::ExtractDisplayName(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        return "Untitled";
    }

    return fs::path(projectFilePath).stem().string();
}
