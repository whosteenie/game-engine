#include "app/project/ProjectSession.h"

#include "app/editor/EditorSettings.h"
#include "app/project/ProjectEditorState.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneImportService.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/project/SceneProjectIO.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/ProjectLoadTrace.h"
#include "engine/rendering/Material.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    bool IsPathInsideOrEqual(const fs::path& candidate, const fs::path& root)
    {
        auto candidateIt = candidate.begin();
        for (const fs::path& rootPart : root)
        {
            if (candidateIt == candidate.end() || *candidateIt != rootPart)
            {
                return false;
            }
            ++candidateIt;
        }
        return true;
    }

    bool CopyProjectAssets(const fs::path& sourceRoot, const fs::path& destinationRoot, std::string& outError)
    {
        const fs::path sourceAssets = sourceRoot / "Assets";
        std::error_code error;
        if (!fs::exists(sourceAssets, error))
        {
            return true;
        }
        if (error || !fs::is_directory(sourceAssets, error))
        {
            outError = "The source project Assets path is not a directory.";
            return false;
        }

        fs::copy(
            sourceAssets,
            destinationRoot / "Assets",
            fs::copy_options::recursive,
            error);
        if (error)
        {
            outError = "Failed to copy the source project Assets folder.";
            return false;
        }
        return true;
    }
}

void ProjectSession::MarkDirty()
{
    m_dirty = true;
}

void ProjectSession::SetStatusMessage(const std::string& message)
{
    m_statusMessage = SanitizeLogText(message, "Internal error.");
}

void ProjectSession::MarkClean()
{
    m_dirty = false;
}

void ProjectSession::CloseProject()
{
    if (m_hasActiveProject)
    {
        EditorSettings::SaveEditorLayout(m_projectRootDirectory);
    }

    Material::ClearTexturePathResolver();
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

bool ProjectSession::CreateNewProject(
    Scene& scene,
    const std::string& directory,
    const std::string& projectName,
    const ProjectEditorState& editorState)
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
    SceneProjectIO::SetMaterialTexturePathResolver(m_projectRootDirectory);

    fs::create_directories(projectDirectory / "Assets", error);
    if (error)
    {
        m_statusMessage = "Failed to create the project Assets folder.";
        CloseProject();
        return false;
    }

    scene.ResetToDefault();

    if (!Save(scene, editorState))
    {
        CloseProject();
        return false;
    }

    m_hasActiveProject = true;
    m_statusMessage = "Created " + m_displayName;
    return true;
}

bool ProjectSession::Save(Scene& scene, const ProjectEditorState& editorState)
{
    if (m_projectFilePath.empty())
    {
        m_statusMessage = "Save requires a project file path. Use Save As.";
        return false;
    }

    std::string error;
    if (!SceneProjectIO::Save(scene, editorState, m_projectRootDirectory, m_projectFilePath, error))
    {
        SetStatusMessage(error.empty() ? "Failed to save project." : error);
        return false;
    }

    MarkClean();
    m_statusMessage = "Saved " + m_displayName;
    return true;
}

bool ProjectSession::SaveAs(Scene& scene, const std::string& projectFilePath, const ProjectEditorState& editorState)
{
    SetProjectFilePath(projectFilePath);

    std::string error;
    if (!SceneProjectIO::Save(scene, editorState, m_projectRootDirectory, m_projectFilePath, error))
    {
        SetStatusMessage(error.empty() ? "Failed to save project." : error);
        return false;
    }

    MarkClean();
    m_statusMessage = "Saved " + m_displayName;
    return true;
}

bool ProjectSession::OpenProject(Scene& scene, const std::string& projectFilePath, ProjectEditorState& editorState)
{
    SetProjectFilePath(projectFilePath);
    ProjectLoadTrace::Step("project paths resolved");

    std::string error;
    ProjectLoadTrace::Scope loadScope("SceneProjectIO::Load");
    const bool loaded = SceneProjectIO::Load(scene, editorState, m_projectRootDirectory, m_projectFilePath, error);
    if (!loaded)
    {
        const std::string loadError = error.empty() ? "Failed to load project." : error;
        SetStatusMessage(loadError);
        ProjectLoadTrace::Step("SceneProjectIO::Load returned false");

        try
        {
            ProjectLoadTrace::Step("ResetToDefault after load failure");
            scene.ResetToDefault();
            ProjectLoadTrace::Step("ResetToDefault after load failure ok");
        }
        catch (const std::exception& exception)
        {
            scene.GetMeshLibrary().InvalidatePrimitives();
            ProjectLoadTrace::Step("ResetToDefault after load failure exception");
            EngineLog::LogFailure(
                "project",
                "ResetToDefault",
                FormatExceptionContext("Failed to reset scene after load error", exception));
        }

        m_hasActiveProject = false;
        return false;
    }
    loadScope.Success();

    ProjectLoadTrace::Scope prewarmScope("Prewarm imported model assets");
    const int warmedModelCount =
        scene.GetImportService().PrewarmProjectModels(scene, m_projectRootDirectory, 0.70f, 0.84f);
    prewarmScope.Success();

    MarkClean();
    m_hasActiveProject = true;
    m_statusMessage = warmedModelCount > 0
        ? "Opened " + m_displayName + " (" + std::to_string(warmedModelCount) + " models ready)"
        : "Opened " + m_displayName;
    return true;
}

bool ProjectSession::DuplicateAsNewProject(
    Scene& scene,
    const std::string& parentDirectory,
    const std::string& projectName,
    const ProjectEditorState& editorState)
{
    if (parentDirectory.empty())
    {
        m_statusMessage = "Choose a parent folder for the new project.";
        return false;
    }

    const std::string sanitizedName = SanitizeProjectName(projectName);
    std::error_code error;
    const fs::path parentPath = fs::absolute(fs::path(parentDirectory), error).lexically_normal();
    if (error || !fs::exists(parentPath, error) || !fs::is_directory(parentPath, error))
    {
        m_statusMessage = "The new project's parent folder does not exist.";
        return false;
    }

    const fs::path destinationRoot = parentPath / sanitizedName;
    if (fs::exists(destinationRoot, error))
    {
        m_statusMessage = "A folder with that project name already exists in the selected location.";
        return false;
    }
    if (error)
    {
        m_statusMessage = "Could not inspect the new project location.";
        return false;
    }

    fs::path sourceRoot;
    if (!m_projectRootDirectory.empty())
    {
        sourceRoot = fs::weakly_canonical(fs::path(m_projectRootDirectory), error);
        if (error)
        {
            m_statusMessage = "Could not resolve the source project folder.";
            return false;
        }
        if (IsPathInsideOrEqual(destinationRoot, sourceRoot))
        {
            m_statusMessage = "Save As cannot create a project inside the source project folder.";
            return false;
        }
    }

    fs::create_directories(destinationRoot, error);
    if (error)
    {
        m_statusMessage = "Failed to create the new project folder.";
        return false;
    }

    std::string copyError;
    if (!sourceRoot.empty() && !CopyProjectAssets(sourceRoot, destinationRoot, copyError))
    {
        fs::remove_all(destinationRoot, error);
        m_statusMessage = copyError;
        return false;
    }
    fs::create_directories(destinationRoot / "Assets", error);
    if (error)
    {
        fs::remove_all(destinationRoot, error);
        m_statusMessage = "Failed to create the new project Assets folder.";
        return false;
    }

    const std::string oldProjectFilePath = m_projectFilePath;
    const std::string oldProjectRootDirectory = m_projectRootDirectory;
    const std::string oldDisplayName = m_displayName;
    const bool oldHasActiveProject = m_hasActiveProject;
    const bool oldDirty = m_dirty;

    SetProjectRootDirectory(destinationRoot.string());
    SetProjectFilePath((destinationRoot / (sanitizedName + ProjectFileExtension)).string());
    SceneProjectIO::SetMaterialTexturePathResolver(m_projectRootDirectory);

    std::string saveError;
    if (!SceneProjectIO::Save(scene, editorState, m_projectRootDirectory, m_projectFilePath, saveError))
    {
        m_projectFilePath = oldProjectFilePath;
        m_projectRootDirectory = oldProjectRootDirectory;
        m_displayName = oldDisplayName;
        m_hasActiveProject = oldHasActiveProject;
        m_dirty = oldDirty;
        SceneProjectIO::SetMaterialTexturePathResolver(m_projectRootDirectory);
        fs::remove_all(destinationRoot, error);
        m_statusMessage = saveError.empty() ? "Failed to save the new project." : saveError;
        return false;
    }

    m_hasActiveProject = true;
    MarkClean();
    m_statusMessage = "Saved new project " + m_displayName;
    return true;
}

void ProjectSession::SetProjectFilePath(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        m_projectFilePath.clear();
        m_displayName = "Untitled";
        m_projectRootDirectory.clear();
        return;
    }

    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(fs::path(projectFilePath), error);
    m_projectFilePath = error ? projectFilePath : canonical.string();
    m_displayName = ExtractDisplayName(m_projectFilePath);

    const fs::path parent = fs::path(m_projectFilePath).parent_path();
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
