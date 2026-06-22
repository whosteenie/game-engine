#include "app/ProjectSession.h"

#include <filesystem>

namespace fs = std::filesystem;

void ProjectSession::MarkDirty()
{
    m_dirty = true;
}

void ProjectSession::MarkClean()
{
    m_dirty = false;
}

void ProjectSession::NewUntitled()
{
    m_projectFilePath.clear();
    m_projectRootDirectory.clear();
    m_displayName = "Untitled";
    m_dirty = false;
    m_statusMessage = "New untitled project.";
}

void ProjectSession::NewAt(const std::string& directory)
{
    const fs::path projectPath = fs::path(directory) / "Untitled.gameproject";
    SetProjectRootDirectory(directory);
    SetProjectFilePath(projectPath.string());
    m_dirty = true;
    m_statusMessage = "New project at " + directory + " (not saved yet).";
}

bool ProjectSession::Save()
{
    if (m_projectFilePath.empty())
    {
        m_statusMessage = "Save requires a project file path. Use Save As.";
        return false;
    }

    m_statusMessage = "Save not implemented yet: " + m_projectFilePath;
    return false;
}

bool ProjectSession::SaveAs(const std::string& projectFilePath)
{
    SetProjectFilePath(projectFilePath);
    m_statusMessage = "Save not implemented yet: " + m_projectFilePath;
    return false;
}

bool ProjectSession::Load(const std::string& projectFilePath)
{
    SetProjectFilePath(projectFilePath);
    m_dirty = false;
    m_statusMessage = "Opened project (load not implemented yet): " + m_projectFilePath;
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
