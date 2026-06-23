#include "app/ProjectChooser.h"

#include "app/EditorSettings.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"
#include "engine/FileDialog.h"

#include <imgui.h>

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

void ProjectChooser::OpenNewProjectForm(EditorSettings& settings)
{
    m_showNewProjectForm = true;
    m_errorMessage.clear();
    std::snprintf(m_newProjectName, sizeof(m_newProjectName), "%s", "My Project");

    settings.ValidateLastNewProjectParentDirectory();
    const std::string& lastParentDirectory = settings.GetLastNewProjectParentDirectory();
    if (lastParentDirectory.empty())
    {
        m_newProjectDirectory[0] = '\0';
    }
    else
    {
        std::snprintf(
            m_newProjectDirectory,
            sizeof(m_newProjectDirectory),
            "%s",
            lastParentDirectory.c_str());
    }
}

bool ProjectChooser::IsBlockingEditor() const
{
    return m_showNewProjectForm || m_startupMode;
}

bool ProjectChooser::TryOpenProject(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const std::string& projectFilePath,
    const ApplyEditorStateFn& applyEditorState,
    std::string& outError)
{
    if (!project.OpenProject(scene, projectFilePath, editorState))
    {
        outError = project.GetStatusMessage();
        return false;
    }

    settings.AddRecentProject(project.GetProjectFilePath());
    settings.SetLastNewProjectParentDirectoryFromProjectFile(project.GetProjectFilePath());
    settings.Save();
    m_startupMode = false;
    m_showNewProjectForm = false;
    m_errorMessage.clear();
    if (applyEditorState)
    {
        applyEditorState(editorState);
    }
    return true;
}

bool ProjectChooser::DrawNewProjectForm(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    const ApplyEditorStateFn& applyEditorState)
{
    const bool startup = m_startupMode;
    const char* popupId = startup ? "Create Project###ProjectChooserCreate" : "New Project###ProjectChooserCreate";

    ImGui::OpenPopup(popupId);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(popupId, nullptr, flags))
    {
        return false;
    }

    ImGui::TextUnformatted("Project name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##ProjectName", m_newProjectName, sizeof(m_newProjectName));

    ImGui::Spacing();
    ImGui::TextUnformatted("Location (parent folder)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText("##ProjectDirectory", m_newProjectDirectory, sizeof(m_newProjectDirectory), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        std::string folderPath;
        if (FileDialog::ChooseProjectFolder(folderPath))
        {
            std::snprintf(m_newProjectDirectory, sizeof(m_newProjectDirectory), "%s", folderPath.c_str());
            settings.SetLastNewProjectParentDirectory(folderPath);
            settings.Save();
        }
    }

    if (!m_errorMessage.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_errorMessage.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)))
    {
        if (m_newProjectDirectory[0] == '\0')
        {
            m_errorMessage = "Choose a location for the new project folder.";
        }
        else if (project.CreateNewProject(
                     scene,
                     m_newProjectDirectory,
                     m_newProjectName,
                     ProjectEditorState::CreateDefault()))
        {
            settings.SetLastNewProjectParentDirectory(m_newProjectDirectory);
            settings.AddRecentProject(project.GetProjectFilePath());
            settings.Save();
            m_startupMode = false;
            m_showNewProjectForm = false;
            m_errorMessage.clear();
            if (applyEditorState)
            {
                applyEditorState(ProjectEditorState::CreateDefault());
            }
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return true;
        }
        else
        {
            m_errorMessage = project.GetStatusMessage();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(startup ? "Back" : "Cancel", ImVec2(120.0f, 0.0f)))
    {
        m_showNewProjectForm = false;
        m_errorMessage.clear();
        ImGui::CloseCurrentPopup();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_showNewProjectForm = false;
        m_errorMessage.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return false;
}

bool ProjectChooser::DrawStartupScreen(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const ApplyEditorStateFn& applyEditorState,
    const RequestCloseCallback& requestClose)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("Project Chooser", nullptr, windowFlags))
    {
        ImGui::End();
        return false;
    }

    const float panelWidth = 520.0f;
    const float panelHeight = 420.0f;
    ImGui::SetCursorPos(ImVec2(
        (viewport->WorkSize.x - panelWidth) * 0.5f,
        (viewport->WorkSize.y - panelHeight) * 0.5f));

    ImGui::BeginChild(
        "ProjectChooserPanel",
        ImVec2(panelWidth, panelHeight),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    ImGui::TextUnformatted("Game Engine Editor");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Open an existing project or create a new one to begin.");
    ImGui::Spacing();

    if (ImGui::Button("Open Project...", ImVec2(-FLT_MIN, 0.0f)))
    {
        settings.ValidateLastNewProjectParentDirectory();
        std::string projectPath;
        if (FileDialog::OpenProjectFile(projectPath, settings.GetLastNewProjectParentDirectory()))
        {
            std::string error;
            if (!TryOpenProject(project, scene, settings, editorState, projectPath, applyEditorState, error))
            {
                m_errorMessage = error;
            }
        }
    }

    if (ImGui::Button("New Project...", ImVec2(-FLT_MIN, 0.0f)))
    {
        OpenNewProjectForm(settings);
    }

    const std::vector<std::string>& recentProjects = settings.GetRecentProjects();
    if (!recentProjects.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Recent Projects");

        for (const std::string& projectPath : recentProjects)
        {
            const std::string label = fs::path(projectPath).stem().string();
            ImGui::PushID(projectPath.c_str());
            if (ImGui::Selectable(label.c_str()))
            {
                std::string error;
                if (!TryOpenProject(project, scene, settings, editorState, projectPath, applyEditorState, error))
                {
                    m_errorMessage = error;
                    settings.RemoveRecentProject(projectPath);
                    settings.Save();
                }
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", projectPath.c_str());
            }

            ImGui::PopID();
        }
    }

    if (!m_errorMessage.empty() && !m_showNewProjectForm)
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_errorMessage.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f)))
    {
        if (requestClose)
        {
            requestClose();
        }
    }

    ImGui::EndChild();
    ImGui::End();

    if (m_showNewProjectForm)
    {
        return DrawNewProjectForm(project, scene, settings, applyEditorState);
    }

    return false;
}

bool ProjectChooser::Draw(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const ApplyEditorStateFn& applyEditorState,
    const RequestCloseCallback& requestClose)
{
    if (project.HasActiveProject() && !m_showNewProjectForm)
    {
        m_startupMode = false;
        return false;
    }

    if (m_showNewProjectForm && !m_startupMode)
    {
        return DrawNewProjectForm(project, scene, settings, applyEditorState);
    }

    return DrawStartupScreen(project, scene, settings, editorState, applyEditorState, requestClose);
}
