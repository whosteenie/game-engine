#include "app/project/ProjectChooser.h"

#include "app/editor/EditorClipboard.h"
#include "app/editor/EditorSettings.h"
#include "app/project/ProjectSession.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/undo/UndoStack.h"
#include "engine/assets/FileDialog.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/NativeProgressWindow.h"
#include "engine/platform/ProjectLoadTrace.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    void DrawErrorText(const std::string& message)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }
}

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

void ProjectChooser::ClearProjectLoadPresentation()
{
    m_startupMode = true;
    m_showNewProjectForm = false;
    m_projectLoadInProgress = false;
    m_finishPresentationAfterPresent = false;
    m_projectLoadFrames = 0;
}

void ProjectChooser::ReturnToStartupWithError(
    ProjectSession& project,
    Scene& scene,
    const std::string& message)
{
    m_lastOpenFailedDueToDeviceRemoved = false;

    if (project.HasActiveProject())
    {
        project.CloseProject();
    }

    std::string deviceRemovedReason;
    const bool deviceRemoved =
        GfxContext::Get().IsInitialized() && GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason);
    if (deviceRemoved)
    {
        m_lastOpenFailedDueToDeviceRemoved = true;
        ClearProjectLoadPresentation();
        m_errorMessage = SanitizeLogText(message, "Project operation failed.");
        return;
    }

    try
    {
        scene.ResetToDefault();
    }
    catch (const std::exception& exception)
    {
        scene.GetMeshLibrary().InvalidatePrimitives();
        EngineLog::LogFailure(
            "project",
            "ReturnToStartupWithError",
            FormatExceptionContext("Failed to reset scene after project error", exception));
    }

    ClearProjectLoadPresentation();
    m_errorMessage = SanitizeLogText(message, "Project operation failed.");
}

bool ProjectChooser::OpenProjectAtPath(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const std::string& projectFilePath,
    const ApplyEditorStateFn& applyEditorState,
    UndoStack& undoStack,
    EditorClipboard& clipboard,
    const FinalizeEditorOpenFn& finalizeEditorOpen,
    std::string& outError)
{
    outError.clear();
    m_lastOpenFailedDueToDeviceRemoved = false;
    ProjectLoadTrace::Reset();
    ProjectLoadTrace::Step("OpenProjectAtPath");

    if (GfxContext::Get().IsInitialized())
    {
        std::string deviceRemovedReason;
        if (GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason))
        {
            outError = "Cannot open project: D3D12 device was removed (" + deviceRemovedReason
                + "). Restart the editor.";
            EngineLog::LogFailure("project", "OpenProject", outError);
            ProjectLoadTrace::Step("device removed — aborting open");
            ReturnToStartupWithError(project, scene, outError);
            return false;
        }
    }

    NativeProgressWindow::Instance().Begin("Loading Project", "Opening project...");
    NativeProgressWindow::Instance().SetProgress(0.02f);
    bool keepProgressOpenForFirstFrame = false;

    try
    {
        if (GfxContext::Get().IsInitialized())
        {
            ProjectLoadTrace::Step("wait for GPU before open");
            NativeProgressWindow::Instance().SetMessage("Finishing previous GPU work...");
            NativeProgressWindow::Instance().SetProgress(0.05f);
            // Do not pump GLFW events here: OpenProjectAtPath runs during Update before
            // the current frame's ImGui NewFrame and resize callbacks can corrupt GPU state.
            GfxContext::Get().WaitForSwapchainFrames(false);

            std::string deviceRemovedReason;
            if (GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason))
            {
                outError = "Cannot open project: D3D12 device was removed (" + deviceRemovedReason
                    + "). Restart the editor.";
                EngineLog::LogFailure("project", "OpenProject", outError);
                ProjectLoadTrace::Step("device removed — aborting open");
                ReturnToStartupWithError(project, scene, outError);
                NativeProgressWindow::Instance().End();
                return false;
            }
        }

        NativeProgressWindow::Instance().SetMessage("Loading project file...");
        NativeProgressWindow::Instance().SetProgress(0.08f);
        ProjectLoadTrace::Scope openProjectScope("ProjectSession::OpenProject");
        if (!project.OpenProject(scene, projectFilePath, editorState))
        {
            outError = SanitizeLogText(project.GetStatusMessage(), "Failed to open project.");
            EngineLog::LogFailure("project", "OpenProject", outError);
            ReturnToStartupWithError(project, scene, outError);
            NativeProgressWindow::Instance().End();
            return false;
        }
        openProjectScope.Success();
        ProjectLoadTrace::Step("scene and project file loaded");
        NativeProgressWindow::Instance().SetProgress(0.78f);

        undoStack.Clear();
        clipboard.Clear();
        ProjectLoadTrace::Step("undo and clipboard cleared");
        NativeProgressWindow::Instance().SetMessage("Saving recent project settings...");
        NativeProgressWindow::Instance().SetProgress(0.82f);
        settings.AddRecentProject(project.GetProjectFilePath());
        settings.SetLastNewProjectParentDirectoryFromProjectFile(project.GetProjectFilePath());
        settings.Save();
        ProjectLoadTrace::Step("editor settings saved");

        if (applyEditorState)
        {
            NativeProgressWindow::Instance().SetMessage("Applying editor preferences...");
            NativeProgressWindow::Instance().SetProgress(0.86f);
            ProjectLoadTrace::Scope editorStateScope("apply editor state");
            try
            {
                applyEditorState(editorState);
            }
            catch (const std::exception& exception)
            {
                outError = FormatExceptionContext("ApplyProjectEditorState", exception);
                EngineLog::LogFailure("project", "ApplyProjectEditorState", outError);
                ReturnToStartupWithError(project, scene, outError);
                NativeProgressWindow::Instance().End();
                return false;
            }
            editorStateScope.Success();
        }

        if (finalizeEditorOpen)
        {
            NativeProgressWindow::Instance().SetMessage("Preparing editor...");
            NativeProgressWindow::Instance().SetProgress(0.90f);
            ProjectLoadTrace::Scope layoutScope("prepare editor open");
            finalizeEditorOpen();
            layoutScope.Success();
        }

        m_showNewProjectForm = false;
        m_errorMessage.clear();
        NativeProgressWindow::Instance().SetMessage("Preparing first frame...");
        NativeProgressWindow::Instance().SetProgress(0.96f);
        m_startupMode = false;
        m_projectLoadInProgress = true;
        m_finishPresentationAfterPresent = false;
        m_projectLoadFrames = 0;
        keepProgressOpenForFirstFrame = true;
        ProjectLoadTrace::Step("=== project load complete (awaiting first frame) ===");
        SceneRenderTrace::Reset();
        ResetDxrBreadcrumbOnceFlags();
        return true;
    }
    catch (const std::exception& exception)
    {
        ProjectLoadTrace::Step("OpenProjectAtPath exception");
        outError = FormatExceptionContext("OpenProjectAtPath", exception);
        EngineLog::LogFailure("project", "OpenProjectAtPath", outError);
        ReturnToStartupWithError(project, scene, outError);
        if (!keepProgressOpenForFirstFrame)
        {
            NativeProgressWindow::Instance().End();
        }
        return false;
    }
    catch (...)
    {
        ProjectLoadTrace::Step("OpenProjectAtPath unknown exception");
        outError = "OpenProjectAtPath: unknown exception";
        EngineLog::LogFailure("project", "OpenProjectAtPath", outError);
        ReturnToStartupWithError(project, scene, outError);
        if (!keepProgressOpenForFirstFrame)
        {
            NativeProgressWindow::Instance().End();
        }
        return false;
    }
}

void ProjectChooser::NotifyEditorCompositeReady()
{
    if (!m_projectLoadInProgress)
    {
        return;
    }

    m_finishPresentationAfterPresent = true;
}

void ProjectChooser::FinishScheduledPresentation()
{
    if (!m_finishPresentationAfterPresent)
    {
        return;
    }

    m_finishPresentationAfterPresent = false;
    FinishProjectLoadPresentation(true);
}

void ProjectChooser::TickProjectLoadTimeout(const bool gpuResourcesFailed)
{
    if (!m_projectLoadInProgress)
    {
        return;
    }

    ++m_projectLoadFrames;

    constexpr int kForceCloseAfterFrames = 300;
    if (gpuResourcesFailed || m_projectLoadFrames >= kForceCloseAfterFrames)
    {
        FinishProjectLoadPresentation(false);
    }
}

void ProjectChooser::FinishProjectLoadPresentation(const bool firstFrameReady)
{
    NativeProgressWindow::Instance().SetMessage(firstFrameReady ? "Project ready." : "Project opened.");
    NativeProgressWindow::Instance().SetProgress(1.0f);
    NativeProgressWindow::Instance().End();
    m_startupMode = false;
    m_showNewProjectForm = false;
    m_projectLoadInProgress = false;
    m_finishPresentationAfterPresent = false;
    m_projectLoadFrames = 0;
}

bool ProjectChooser::QueueProjectOpen(const std::string& projectFilePath)
{
    if (projectFilePath.empty())
    {
        m_errorMessage = "Project path is empty.";
        return false;
    }

    m_pendingProjectPath = projectFilePath;
    m_errorMessage.clear();
    return true;
}

bool ProjectChooser::TryOpenProject(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const std::string& projectFilePath,
    const ApplyEditorStateFn& applyEditorState,
    UndoStack& undoStack,
    EditorClipboard& clipboard,
    std::string& outError)
{
    (void)project;
    (void)scene;
    (void)settings;
    (void)editorState;
    (void)applyEditorState;
    (void)undoStack;
    (void)clipboard;
    if (!QueueProjectOpen(projectFilePath))
    {
        outError = m_errorMessage;
        return false;
    }

    outError.clear();
    return true;
}

bool ProjectChooser::ProcessPendingProjectOpen(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const ApplyEditorStateFn& applyEditorState,
    UndoStack& undoStack,
    EditorClipboard& clipboard,
    const FinalizeEditorOpenFn& finalizeEditorOpen,
    std::string& outError)
{
    if (m_pendingProjectPath.empty())
    {
        return false;
    }

    const std::string projectFilePath = std::move(m_pendingProjectPath);
    m_pendingProjectPath.clear();
    return OpenProjectAtPath(
        project,
        scene,
        settings,
        editorState,
        projectFilePath,
        applyEditorState,
        undoStack,
        clipboard,
        finalizeEditorOpen,
        outError);
}

bool ProjectChooser::DrawNewProjectForm(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    const ApplyEditorStateFn& applyEditorState,
    const FinalizeEditorOpenFn& prepareEditorOpen,
    UndoStack& undoStack,
    EditorClipboard& clipboard)
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

    bool projectCreated = false;
    ProjectEditorState createdEditorState = ProjectEditorState::CreateDefault();
    bool dismissForm = false;

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
        DrawErrorText(m_errorMessage);
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
            projectCreated = true;
            createdEditorState = ProjectEditorState::CreateDefault();
        }
        else
        {
            m_errorMessage = project.GetStatusMessage();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(startup ? "Back" : "Cancel", ImVec2(120.0f, 0.0f)))
    {
        dismissForm = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        dismissForm = true;
    }

    ImGui::EndPopup();

    if (dismissForm)
    {
        m_showNewProjectForm = false;
        m_errorMessage.clear();
        return false;
    }

    if (!projectCreated)
    {
        return false;
    }

    settings.SetLastNewProjectParentDirectory(m_newProjectDirectory);
    settings.AddRecentProject(project.GetProjectFilePath());
    settings.Save();
    undoStack.Clear();
    clipboard.Clear();

    if (applyEditorState)
    {
        try
        {
            applyEditorState(createdEditorState);
        }
        catch (const std::exception& exception)
        {
            const std::string applyError = FormatExceptionContext("ApplyProjectEditorState", exception);
            EngineLog::LogFailure("project", "ApplyProjectEditorState", applyError);
            ReturnToStartupWithError(project, scene, applyError);
            return false;
        }
    }

    if (prepareEditorOpen)
    {
        prepareEditorOpen();
    }

    m_startupMode = false;
    m_showNewProjectForm = false;
    m_errorMessage.clear();
    EngineLog::Info("project", "Project created: " + project.GetProjectFilePath());
    return true;
}

bool ProjectChooser::DrawStartupScreen(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const ApplyEditorStateFn& applyEditorState,
    const RequestCloseCallback& requestClose,
    const FinalizeEditorOpenFn& prepareEditorOpen,
    UndoStack& undoStack,
    EditorClipboard& clipboard)
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
    const float mainPanelHeight = 400.0f;
    const float errorPanelMaxHeight = 120.0f;

    const bool showStartupError = !m_errorMessage.empty() && !m_showNewProjectForm;
    const float groupHeight = mainPanelHeight + (showStartupError ? (ImGui::GetStyle().ItemSpacing.y + errorPanelMaxHeight) : 0.0f);
    ImGui::SetCursorPos(ImVec2(
        (viewport->WorkSize.x - panelWidth) * 0.5f,
        std::max((viewport->WorkSize.y - groupHeight) * 0.5f, ImGui::GetStyle().WindowPadding.y)));

    ImGui::BeginGroup();

    ImGui::BeginChild(
        "ProjectChooserPanel",
        ImVec2(panelWidth, mainPanelHeight),
        ImGuiChildFlags_Borders);

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
                if (!TryOpenProject(
                    project,
                    scene,
                    settings,
                    editorState,
                    projectPath,
                    applyEditorState,
                    undoStack,
                    clipboard,
                    error))
                {
                    m_errorMessage = error.empty() ? "Failed to open project." : error;
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

        const float recentListHeight = std::max(
            80.0f,
            mainPanelHeight - ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing() * 3.0f);
        ImGui::BeginChild("ProjectChooserRecentList", ImVec2(0.0f, recentListHeight), ImGuiChildFlags_None);

        for (const std::string& projectPath : recentProjects)
        {
            const std::string label = fs::path(projectPath).stem().string();
            ImGui::PushID(projectPath.c_str());
            if (ImGui::Selectable(label.c_str()))
            {
                std::string error;
                if (!TryOpenProject(
                    project,
                    scene,
                    settings,
                    editorState,
                    projectPath,
                    applyEditorState,
                    undoStack,
                    clipboard,
                    error))
                {
                    m_errorMessage = error.empty() ? "Failed to open project." : error;
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

        ImGui::EndChild();
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

    if (showStartupError)
    {
        ImGui::Spacing();
        ImGui::BeginChild(
            "ProjectChooserError",
            ImVec2(panelWidth, errorPanelMaxHeight),
            ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Last error");
        ImGui::Separator();
        DrawErrorText(m_errorMessage);
        ImGui::EndChild();
    }

    ImGui::EndGroup();
    ImGui::End();

    if (m_showNewProjectForm)
    {
        return DrawNewProjectForm(
            project,
            scene,
            settings,
            applyEditorState,
            prepareEditorOpen,
            undoStack,
            clipboard);
    }

    return false;
}

bool ProjectChooser::Draw(
    ProjectSession& project,
    Scene& scene,
    EditorSettings& settings,
    ProjectEditorState& editorState,
    const ApplyEditorStateFn& applyEditorState,
    const RequestCloseCallback& requestClose,
    const FinalizeEditorOpenFn& prepareEditorOpen,
    UndoStack& undoStack,
    EditorClipboard& clipboard)
{
    if (project.HasActiveProject() && !m_startupMode && !m_showNewProjectForm)
    {
        return false;
    }

    if (!project.HasActiveProject())
    {
        m_startupMode = true;
    }

    if (m_showNewProjectForm && !m_startupMode)
    {
        return DrawNewProjectForm(
            project,
            scene,
            settings,
            applyEditorState,
            prepareEditorOpen,
            undoStack,
            clipboard);
    }

    return DrawStartupScreen(
        project,
        scene,
        settings,
        editorState,
        applyEditorState,
        requestClose,
        prepareEditorOpen,
        undoStack,
        clipboard);
}
