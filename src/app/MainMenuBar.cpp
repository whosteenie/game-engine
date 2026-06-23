#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/EditorSettings.h"
#include "app/MainMenuBar.h"
#include "app/ProjectEditorState.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"
#include "app/UndoCommand.h"
#include "app/UndoContext.h"
#include "app/SceneDocument.h"
#include "app/UndoStack.h"
#include "engine/FileDialog.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    void DrawAboutPopup()
    {
        if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Game Engine Editor");
            ImGui::Separator();
            ImGui::TextUnformatted("Projects are saved as JSON .gameproject files.");
            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void DrawPanelToggle(const char* label, bool* visible)
    {
        if (visible == nullptr)
        {
            return;
        }

        ImGui::MenuItem(label, nullptr, visible);
    }

    void ImportModelIntoScene(
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        int parentIndex)
    {
        std::string modelPath;
        if (!FileDialog::OpenModelFile(modelPath))
        {
            return;
        }

        const std::string& projectRoot = project.GetProjectRootDirectory();
        SceneDocument before = SceneDocument::Capture(scene, projectRoot);
        const std::vector<int> importedIndices =
            scene.ImportModel(modelPath, parentIndex, projectRoot);
        if (importedIndices.empty())
        {
            if (!scene.GetLastImportError().empty())
            {
                project.SetStatusMessage(scene.GetLastImportError());
            }
            return;
        }

        scene.SetSelectedObjectIndex(importedIndices.front());
        SceneDocument after = SceneDocument::Capture(scene, projectRoot);
        undoStack.Push(std::make_unique<ApplySceneDocumentCommand>(
            std::move(before),
            std::move(after),
            "Import Model",
            projectRoot));

        if (!scene.GetLastImportWarning().empty())
        {
            project.SetStatusMessage(scene.GetLastImportWarning());
        }
    }

    void RecordRecentProject(EditorSettings& settings, const std::string& projectFilePath)
    {
        settings.AddRecentProject(projectFilePath);
        settings.SetLastNewProjectParentDirectoryFromProjectFile(projectFilePath);
        settings.Save();
    }

    void OpenProject(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack)
    {
        settings.ValidateLastNewProjectParentDirectory();
        std::string projectPath;
        if (!FileDialog::OpenProjectFile(projectPath, settings.GetLastNewProjectParentDirectory()))
        {
            return;
        }

        if (project.OpenProject(scene, projectPath, editorState))
        {
            undoStack.Clear();
            RecordRecentProject(settings, project.GetProjectFilePath());
            if (applyEditorState)
            {
                applyEditorState(editorState);
            }
        }
    }

    void SaveProject(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const CaptureEditorStateFn& captureEditorState)
    {
        if (project.IsUntitled() || !project.IsDirty())
        {
            return;
        }

        if (captureEditorState)
        {
            captureEditorState(editorState);
        }

        if (!project.Save(scene, editorState))
        {
            std::string projectPath;
            if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
            {
                if (project.SaveAs(scene, projectPath, editorState))
                {
                    RecordRecentProject(settings, project.GetProjectFilePath());
                }
            }
        }
    }

    void SaveProjectAs(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const CaptureEditorStateFn& captureEditorState)
    {
        std::string projectPath;
        if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
        {
            if (captureEditorState)
            {
                captureEditorState(editorState);
            }

            if (project.SaveAs(scene, projectPath, editorState))
            {
                RecordRecentProject(settings, project.GetProjectFilePath());
            }
        }
    }

    bool AllowFileMenuShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        return !io.WantTextInput && !ImGui::IsAnyItemActive();
    }

    void HandleFileMenuShortcuts(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const CaptureEditorStateFn& captureEditorState,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            OpenProject(scene, project, settings, editorState, applyEditorState, undoStack);
        }

        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveProject(scene, project, settings, editorState, captureEditorState);
        }

        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveProjectAs(scene, project, settings, editorState, captureEditorState);
        }
    }

    void HandleEditMenuShortcuts(Scene& scene, ProjectSession& project, UndoStack& undoStack)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (!io.KeyCtrl)
        {
            return;
        }

        UndoContext context{scene, project.GetProjectRootDirectory()};
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            if (undoStack.CanRedo())
            {
                undoStack.Redo(context);
            }
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
        {
            if (undoStack.CanRedo())
            {
                undoStack.Redo(context);
            }
        }
        else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            if (undoStack.CanUndo())
            {
                undoStack.Undo(context);
            }
        }
    }
}

void MainMenuBar::Draw(
    Scene& scene,
    ProjectSession& project,
    EditorSettings& settings,
    GLFWwindow* window,
    const EditorPanelVisibility& panels,
    ProjectEditorState& editorState,
    const CaptureEditorStateFn& captureEditorState,
    const ApplyEditorStateFn& applyEditorState,
    const std::function<void()>& requestClose,
    const std::function<void()>& requestNewProject,
    UndoStack& undoStack)
{
    HandleFileMenuShortcuts(
        scene,
        project,
        settings,
        editorState,
        captureEditorState,
        applyEditorState,
        undoStack);
    HandleEditMenuShortcuts(scene, project, undoStack);

    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
        {
            if (requestNewProject)
            {
                requestNewProject();
            }
        }

        if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
        {
            OpenProject(scene, project, settings, editorState, applyEditorState, undoStack);
        }

        ImGui::Separator();

        const bool canSave = !project.IsUntitled() && project.IsDirty();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, canSave))
        {
            SaveProject(scene, project, settings, editorState, captureEditorState);
        }

        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
        {
            SaveProjectAs(scene, project, settings, editorState, captureEditorState);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Import Model..."))
        {
            ImportModelIntoScene(scene, project, undoStack, -1);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Exit", "Alt+F4"))
        {
            if (requestClose)
            {
                requestClose();
            }
            else
            {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        UndoContext context{scene, project.GetProjectRootDirectory()};
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, undoStack.CanUndo()))
        {
            undoStack.Undo(context);
        }

        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, undoStack.CanRedo()))
        {
            undoStack.Redo(context);
        }

        ImGui::Separator();

        ImGui::BeginDisabled();
        ImGui::MenuItem("Preferences...");
        ImGui::EndDisabled();

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        DrawPanelToggle("Hierarchy", panels.hierarchy);
        DrawPanelToggle("Inspector", panels.inspector);
        DrawPanelToggle("Toolbar", panels.toolbar);
        DrawPanelToggle("Renderer Tuning", panels.lighting);
        DrawPanelToggle("Project", panels.project);

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About"))
        {
            ImGui::OpenPopup("About");
        }

        ImGui::EndMenu();
    }

    const char* dirtySuffix = project.IsDirty() ? "*" : "";
    const std::string projectLabel = project.GetDisplayName() + dirtySuffix;
    const float statusWidth = ImGui::CalcTextSize(projectLabel.c_str()).x;
    const float messageWidth =
        project.GetStatusMessage().empty() ? 0.0f
                                           : ImGui::CalcTextSize(project.GetStatusMessage().c_str()).x + 24.0f;
    const float rightPadding = 12.0f;
    const float rightBlockWidth = statusWidth + messageWidth + rightPadding;

    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rightBlockWidth);
    ImGui::TextDisabled("%s", projectLabel.c_str());

    if (!project.GetStatusMessage().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", project.GetStatusMessage().c_str());
    }

    ImGui::EndMainMenuBar();

    DrawAboutPopup();
}
