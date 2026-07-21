#include "app/core/Application.h"
#include "app/editor/EditorDockSpace.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/MainMenuBar.h"
#include "app/panels/GameViewportPanel.h"
#include "app/panels/LightingPanel.h"
#include "app/panels/ProjectFilesPanel.h"
#include "app/panels/SceneHierarchyPanel.h"
#include "app/panels/SceneInspectorPanel.h"
#include "app/panels/SceneToolbarPanel.h"
#include "app/panels/SceneViewportPanel.h"
#include "app/project/ProjectChooser.h"
#include "app/project/ProjectEditorState.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "app/scene/editing/SceneEditingController.h"
#include "app/scene/editing/SceneEditor.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/assets/TextureCache.h"
#include "engine/assets/FileDialog.h"
#include "engine/camera/Camera.h"
#include "engine/platform/input/Input.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

void Application::UpdatePendingProjectStartupProgress(const char* message) const
{
    if (message == nullptr)
    {
        return;
    }

    if (NativeProgressWindow::Instance().IsActive())
    {
        NativeProgressWindow::Instance().SetMessage(message);
    }
    if (m_window != nullptr)
    {
        glfwPollEvents();
    }
}

void Application::ProcessQueuedProjectOpenIfReady()
{
    if (m_projectChooser == nullptr || m_projectSession == nullptr || m_scene == nullptr
        || m_editorSettings == nullptr || !m_projectChooser->HasPendingProjectOpen())
    {
        return;
    }

    std::string pendingProjectError;
    const bool openedProject = m_projectChooser->ProcessPendingProjectOpen(
        *m_projectSession,
        *m_scene,
        *m_editorSettings,
        m_projectEditorState,
        [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
        m_undoStack,
        m_editorClipboard,
        [this]() { ResetEditorLayoutLoadState(); },
        pendingProjectError);
    if (!openedProject && !pendingProjectError.empty())
    {
        if (m_projectChooser->LastOpenFailedDueToDeviceRemoved())
        {
            HandleFatalGpuDeviceLoss(pendingProjectError);
            return;
        }

        m_projectChooser->SetErrorMessage(pendingProjectError);
    }
}

void Application::ProcessPendingProjectTeardown()
{
    if (!m_pendingProjectTeardown || m_scene == nullptr)
    {
        return;
    }

    // Project discard is requested while ImGui is building the unsaved-changes dialog. Defer the
    // actual teardown to the next frame boundary so no in-flight scene work or UI draw data can
    // still reference resources owned by the discarded project.
    m_pendingProjectTeardown = false;
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().WaitForSwapchainFrames(false);
        DlssContext::Get().ReleaseViewportResources(0);
        DlssContext::Get().ReleaseViewportResources(1);
    }

    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().SetActiveMsaaSampleCount(1);
    }
    m_scene->ResetForProjectTransition();
    m_scene->ResetToDefault();
    TextureCache::Get().Clear();
    m_sceneEditingController->GetEditor().ResetInteractionState();
    m_playModeDiscardUndoStack.Clear();
    m_wasPlayModeActive = false;
    m_input->ReleaseMouseCapture();
}

void Application::RequestClose()
{
    if (!m_projectSession->HasActiveProject() || !m_projectSession->IsDirty())
    {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        return;
    }

    m_pendingClose = true;
}

void Application::RequestForcedClose()
{
    EngineLog::Breadcrumb("application", "Console close requested; skipping unsaved-project prompt.");
    m_pendingClose = false;
    m_pendingNewProject = false;
    m_pendingOpenProject = false;
    if (m_input != nullptr)
    {
        m_input->ReleaseMouseCapture();
    }
    if (m_window != nullptr)
    {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
    }
}

void Application::RequestNewProject()
{
    if (m_playModeController.IsActive())
    {
        m_playModeController.TogglePlayStop(*m_scene, m_projectSession->GetProjectRootDirectory());
    }

    if (m_projectSession->IsDirty())
    {
        m_pendingNewProject = true;
        return;
    }

    m_projectChooser->OpenNewProjectForm(*m_editorSettings);
}

void Application::RequestOpenProject()
{
    if (m_playModeController.IsActive())
    {
        m_playModeController.TogglePlayStop(*m_scene, m_projectSession->GetProjectRootDirectory());
    }

    if (m_projectSession->IsDirty())
    {
        m_pendingOpenProject = true;
        return;
    }

    m_mainMenuBar->ShowOpenProjectModal();
}

bool Application::IsEditorUndoRedoBlocked() const
{
    return m_pendingClose || m_pendingNewProject || m_pendingOpenProject;
}

Scene* Application::GetEditorTargetScene()
{
    if (m_playModeController.IsActive())
    {
        Scene* runtimeScene = m_playModeController.GetRuntimeScene();
        if (runtimeScene != nullptr)
        {
            return runtimeScene;
        }
    }

    return m_scene.get();
}

const Scene* Application::GetEditorTargetScene() const
{
    return const_cast<Application*>(this)->GetEditorTargetScene();
}

UndoStack* Application::GetEditorUndoStack()
{
    if (m_playModeController.IsActive())
    {
        return &m_playModeDiscardUndoStack;
    }

    return &m_undoStack;
}

const UndoStack* Application::GetEditorUndoStack() const
{
    return const_cast<Application*>(this)->GetEditorUndoStack();
}

void Application::CaptureProjectEditorState(ProjectEditorState& editorState) const
{
    editorState.cameraPosition = m_camera->GetPosition();
    editorState.cameraYaw = m_camera->GetYaw();
    editorState.cameraPitch = m_camera->GetPitch();
    editorState.showHierarchy = m_sceneHierarchyPanel->ShowPanel();
    editorState.showInspector = m_sceneInspectorPanel->ShowPanel();
    editorState.showToolbar = m_sceneToolbarPanel->ShowPanel();
    editorState.showLighting = m_lightingPanel->ShowPanel();
    editorState.showPerformance = m_performancePanel->ShowPanel();
    editorState.performanceGpuPassSmoothing = m_performancePanel->GpuPassSmoothingEnabled();
    editorState.performanceCpuPassSmoothing = m_performancePanel->CpuPassSmoothingEnabled();
    editorState.showProjectFiles = m_projectFilesPanel->ShowPanel();
    editorState.showSceneView = m_sceneViewportPanel->ShowPanel();
    editorState.showGameView = m_gameViewportPanel->ShowPanel();
    editorState.hierarchyNodeOpenStates = m_sceneHierarchyPanel->GetNodeOpenStates();
    m_projectFilesPanel->GetBrowseState(
        editorState.projectFilesBrowsedDirectory,
        editorState.projectFilesSelectedPath,
        editorState.projectFilesFolderOpenStates);
}

void Application::ApplyProjectEditorState(const ProjectEditorState& editorState)
{
    m_camera->SetPosition(editorState.cameraPosition);
    m_camera->SetOrientation(editorState.cameraYaw, editorState.cameraPitch);

    m_sceneHierarchyPanel->ShowPanel() = editorState.showHierarchy;
    m_sceneInspectorPanel->ShowPanel() = editorState.showInspector;
    m_sceneToolbarPanel->ShowPanel() = editorState.showToolbar;
    m_lightingPanel->ShowPanel() = editorState.showLighting;
    m_performancePanel->ShowPanel() = editorState.showPerformance;
    m_performancePanel->GpuPassSmoothingEnabled() = editorState.performanceGpuPassSmoothing;
    m_performancePanel->CpuPassSmoothingEnabled() = editorState.performanceCpuPassSmoothing;
    m_projectFilesPanel->ShowPanel() = editorState.showProjectFiles;
    m_sceneViewportPanel->ShowPanel() = editorState.showSceneView;
    m_gameViewportPanel->ShowPanel() = editorState.showGameView;
    if (m_automationDualViewportLayout)
    {
        m_sceneViewportPanel->ShowPanel() = true;
        m_gameViewportPanel->ShowPanel() = true;
    }

    std::unordered_map<SceneObjectId, bool> hierarchyOpenStates;
    const int objectCount = static_cast<int>(m_scene->GetObjects().size());
    for (const auto& [storedKey, isOpen] : editorState.hierarchyNodeOpenStates)
    {
        if (!isOpen)
        {
            continue;
        }

        const int indexById = m_scene->FindObjectIndex(storedKey);
        if (indexById >= 0)
        {
            hierarchyOpenStates[storedKey] = true;
            continue;
        }

        const int legacyIndex = static_cast<int>(storedKey);
        if (legacyIndex >= 0 && legacyIndex < objectCount)
        {
            hierarchyOpenStates[m_scene->GetSceneObject(static_cast<std::size_t>(legacyIndex)).GetId()] = true;
        }
    }
    m_sceneHierarchyPanel->SetNodeOpenStates(hierarchyOpenStates);

    std::string browsedDirectory = editorState.projectFilesBrowsedDirectory;
    if (browsedDirectory.empty())
    {
        browsedDirectory = m_projectSession->GetProjectRootDirectory();
    }

    m_projectFilesPanel->SetBrowseState(
        browsedDirectory,
        editorState.projectFilesSelectedPath,
        editorState.projectFilesFolderOpenStates);
}

bool Application::TrySaveProject()
{
    CaptureProjectEditorState(m_projectEditorState);

    if (!m_projectSession->IsUntitled())
    {
        if (!m_projectSession->Save(*m_scene, m_projectEditorState))
        {
            return false;
        }

        EditorSettings::SaveEditorLayout(m_projectSession->GetProjectRootDirectory());
        return true;
    }

    std::string projectPath;
    if (!FileDialog::SaveProjectFile(projectPath, m_projectSession->GetProjectFilePath()))
    {
        return false;
    }

    if (!m_projectSession->SaveAs(*m_scene, projectPath, m_projectEditorState))
    {
        return false;
    }

    m_editorSettings->AddRecentProject(m_projectSession->GetProjectFilePath());
    m_editorSettings->SetLastNewProjectParentDirectoryFromProjectFile(m_projectSession->GetProjectFilePath());
    m_editorSettings->Save();
    EditorSettings::SaveEditorLayout(
        m_projectSession != nullptr && m_projectSession->HasActiveProject()
            ? m_projectSession->GetProjectRootDirectory()
            : std::string{});
    return true;
}

void Application::ResetEditorLayout()
{
    EditorSettings::DeleteGlobalImGuiIni();
    m_editorDockSpace->ResetLayout();
    m_editorLayoutRestoredFromDisk = false;
    m_pendingEditorLayoutValidation = false;
}

void Application::ResetEditorLayoutLoadState()
{
    m_globalEditorLayoutLoaded = false;
    m_editorLayoutRestoredFromDisk = false;
    m_pendingEditorLayoutValidation = false;
    // The first restored-layout frame applies saved absolute dock sizes; the following frame fits
    // them to the current host viewport. Do not allocate or render a project-sized framebuffer
    // until both passes have completed.
    m_editorLayoutStabilizationFrames = 2;
    m_editorDockSpace->InvalidateBuiltLayout();
    m_sceneViewportPanel->InvalidateCompositeFrame();
    m_gameViewportPanel->InvalidateCompositeFrame();
}

void Application::EnsureEditorLayoutLoaded()
{
    if (m_globalEditorLayoutLoaded)
    {
        return;
    }

    try
    {
        ImGui::ClearIniSettings();
        m_editorDockSpace->InvalidateBuiltLayout();
        m_editorLayoutRestoredFromDisk = EditorSettings::LoadEditorLayout(
            m_projectSession->HasActiveProject() ? m_projectSession->GetProjectRootDirectory() : std::string{});
        if (!m_editorLayoutRestoredFromDisk
            && m_projectSession->HasActiveProject()
            && EditorSettings::TryMigrateProjectEditorLayout(m_projectSession->GetProjectRootDirectory()))
        {
            m_editorLayoutRestoredFromDisk = true;
        }

        if (!m_editorLayoutRestoredFromDisk)
        {
            EngineLog::Warn("editor", "No saved editor layout found; using default layout.");
            m_editorDockSpace->RequestLayoutRebuild();
        }
        else
        {
            m_pendingEditorLayoutValidation = true;
        }
    }
    catch (const std::exception& exception)
    {
        EngineLog::LogException("editor", "LoadGlobalEditorLayout", exception);
        EditorSettings::DeleteGlobalImGuiIni();
        m_editorDockSpace->ResetLayout();
        m_editorLayoutRestoredFromDisk = false;
        m_pendingEditorLayoutValidation = false;
    }

    m_globalEditorLayoutLoaded = true;
}

void Application::DrawUnsavedChangesDialog()
{
    if (!m_pendingClose && !m_pendingNewProject && !m_pendingOpenProject)
    {
        return;
    }

    const bool isOpenPrompt = m_pendingOpenProject;
    const bool isClosePrompt = m_pendingClose;
    const char* popupId = isOpenPrompt ? "Unsaved Changes###OpenProject" : "Unsaved Changes";
    ImGui::OpenPopup(popupId);

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(
            popupId,
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    }

    if (isClosePrompt)
    {
        ImGui::TextUnformatted("Save changes before closing?");
    }
    else if (isOpenPrompt)
    {
        ImGui::TextUnformatted("Save changes before opening another project?");
    }
    else
    {
        ImGui::TextUnformatted("Save changes before creating a new project?");
    }

    ImGui::Separator();

    auto continueAfterDecision = [&]() {
        if (isOpenPrompt)
        {
            // Keep this popup open. The picker uses the same ID and replaces only the contents
            // next frame, while applying its own size and centered position.
            m_pendingOpenProject = false;
            m_mainMenuBar->ShowOpenProjectModal();
            return;
        }

        ImGui::CloseCurrentPopup();
        m_pendingNewProject = false;
        m_undoStack.Clear();
        m_editorClipboard.Clear();
        m_projectSession->CloseProject();
        m_pendingProjectTeardown = true;
        m_projectChooser->OpenNewProjectForm(*m_editorSettings);
    };

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
    {
        if (TrySaveProject())
        {
            if (isClosePrompt)
            {
                m_pendingClose = false;
                ImGui::CloseCurrentPopup();
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            else
            {
                continueAfterDecision();
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(120.0f, 0.0f)))
    {
        if (isClosePrompt)
        {
            m_pendingClose = false;
            ImGui::CloseCurrentPopup();
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        }
        else
        {
            continueAfterDecision();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
    {
        m_pendingClose = false;
        m_pendingNewProject = false;
        m_pendingOpenProject = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


