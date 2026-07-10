#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/editor/EditorClipboard.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/MainMenuBar.h"
#include "app/core/PlayModeController.h"
#include "app/project/ProjectEditorState.h"
#include "app/project/ProjectSession.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneImportService.h"
#include "app/undo/UndoCommand.h"
#include "app/undo/UndoContext.h"
#include "app/undo/UndoStack.h"
#include "engine/assets/FileDialog.h"

#include <imgui.h>

#include <filesystem>
#include <cstdio>
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
        PushInsertSubtree(undoStack, scene, "Import Model", [&](Scene& target) {
            const std::vector<int> importedIndices =
                target.ImportModel(modelPath, parentIndex, projectRoot);
            if (!importedIndices.empty())
            {
                target.SelectSingle(importedIndices.front());
            }

            return importedIndices;
        });

        if (!scene.GetImportService().GetLastImportError().empty())
        {
            project.SetStatusMessage(scene.GetImportService().GetLastImportError());
        }
        else if (!scene.GetImportService().GetLastImportWarning().empty())
        {
            project.SetStatusMessage(scene.GetImportService().GetLastImportWarning());
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
        UndoStack& undoStack,
        EditorClipboard& clipboard)
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
            clipboard.Clear();
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
        UndoStack& undoStack,
        EditorClipboard& clipboard)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            OpenProject(scene, project, settings, editorState, applyEditorState, undoStack, clipboard);
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

    void PasteClipboardDefault(
        UndoStack& undoStack,
        Scene& scene,
        const EditorClipboard& clipboard)
    {
        const int referenceIndex = scene.HasSelection() ? scene.GetPrimarySelection() : -1;
        PushPasteFromClipboard(
            undoStack,
            scene,
            clipboard,
            referenceIndex,
            HierarchyInsertMode::After);
    }

    void HandleEditMenuShortcuts(
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        bool allowUndoRedo)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && scene.HasSelection())
        {
            CopySelection(clipboard, scene);
            return;
        }

        if (!allowUndoRedo)
        {
            return;
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && scene.HasSelection())
        {
            CutSelection(undoStack, clipboard, scene);
            return;
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && clipboard.HasContent())
        {
            PasteClipboardDefault(undoStack, scene, clipboard);
            return;
        }

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

    void HandleGameMenuShortcuts(Scene& scene, ProjectSession& project, PlayModeController& playMode)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_P, false))
        {
            if (!playMode.TogglePlayStop(scene, project.GetProjectRootDirectory())
                && !playMode.GetLastError().empty())
            {
                project.SetStatusMessage(playMode.GetLastError());
            }
        }
    }

    void HandleGameObjectMenuShortcuts(const std::function<void()>& alignSelectionToView)
    {
        if (!AllowFileMenuShortcuts() || !alignSelectionToView)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            alignSelectionToView();
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
    const std::function<void()>& requestResetLayout,
    const std::function<void()>& alignSelectionToView,
    PlayModeController& playMode,
    UndoStack& undoStack,
    EditorClipboard& clipboard,
    bool allowUndoRedo)
{
    HandleFileMenuShortcuts(
        scene,
        project,
        settings,
        editorState,
        captureEditorState,
        applyEditorState,
        undoStack,
        clipboard);
    HandleEditMenuShortcuts(scene, project, undoStack, clipboard, allowUndoRedo);
    HandleGameMenuShortcuts(scene, project, playMode);
    HandleGameObjectMenuShortcuts(alignSelectionToView);

    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
        {
            if (playMode.IsActive())
            {
                playMode.TogglePlayStop(scene, project.GetProjectRootDirectory());
            }

            if (requestNewProject)
            {
                requestNewProject();
            }
        }

        if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
        {
            if (playMode.IsActive())
            {
                playMode.TogglePlayStop(scene, project.GetProjectRootDirectory());
            }

            OpenProject(scene, project, settings, editorState, applyEditorState, undoStack, clipboard);
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

        char undoLabel[256];
        if (undoStack.CanUndo())
        {
            std::snprintf(undoLabel, sizeof(undoLabel), "Undo %s", undoStack.GetUndoName());
        }
        else
        {
            std::snprintf(undoLabel, sizeof(undoLabel), "Undo");
        }

        char redoLabel[256];
        if (undoStack.CanRedo())
        {
            std::snprintf(redoLabel, sizeof(redoLabel), "Redo %s", undoStack.GetRedoName());
        }
        else
        {
            std::snprintf(redoLabel, sizeof(redoLabel), "Redo");
        }

        const bool canUndo = allowUndoRedo && undoStack.CanUndo();
        const bool canRedo = allowUndoRedo && undoStack.CanRedo();

        if (ImGui::MenuItem(undoLabel, "Ctrl+Z", false, canUndo))
        {
            undoStack.Undo(context);
        }

        if (ImGui::MenuItem(redoLabel, "Ctrl+Y, Ctrl+Shift+Z", false, canRedo))
        {
            undoStack.Redo(context);
        }

        ImGui::Separator();

        const bool canCopy = scene.HasSelection();
        const bool canCut = allowUndoRedo && canCopy;
        const bool canPaste = allowUndoRedo && clipboard.HasContent();

        if (ImGui::MenuItem("Cut", "Ctrl+X", false, canCut))
        {
            CutSelection(undoStack, clipboard, scene);
        }

        if (ImGui::MenuItem("Copy", "Ctrl+C", false, canCopy))
        {
            CopySelection(clipboard, scene);
        }

        if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste))
        {
            PasteClipboardDefault(undoStack, scene, clipboard);
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
        DrawPanelToggle("Scene View", panels.sceneView);
        DrawPanelToggle("Game View", panels.gameView);
        DrawPanelToggle("Toolbar", panels.toolbar);
        DrawPanelToggle("Renderer Tuning", panels.lighting);
        DrawPanelToggle("Performance", panels.performance);
        DrawPanelToggle("Project", panels.project);

        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            if (requestResetLayout)
            {
                requestResetLayout();
            }
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("GameObject"))
    {
        if (ImGui::MenuItem("Align to View", "Ctrl+Shift+F"))
        {
            if (alignSelectionToView)
            {
                alignSelectionToView();
            }
        }

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
