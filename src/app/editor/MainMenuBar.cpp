#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/editor/EditorClipboard.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/MainMenuBar.h"
#include "app/editor/TuningSectionState.h"
#include "app/editor/SettingRegistry.h"
#include "app/core/PlayModeController.h"
#include "app/project/ProjectEditorState.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "app/scene/import/SceneImportService.h"
#include "app/undo/UndoCommand.h"
#include "app/undo/UndoContext.h"
#include "app/undo/UndoStack.h"
#include "engine/assets/FileDialog.h"

#include <imgui.h>

#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    void DrawAboutPopup()
    {
        if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Who Engine Editor");
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

    bool AllowFileMenuShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        return !io.WantTextInput && !ImGui::IsAnyItemActive();
    }

    enum class FileMenuShortcut
    {
        None,
        OpenProject,
        SaveAs,
    };

    FileMenuShortcut HandleFileMenuShortcuts(
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
            return FileMenuShortcut::None;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            return FileMenuShortcut::OpenProject;
        }

        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveProject(scene, project, settings, editorState, captureEditorState);
        }

        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            return FileMenuShortcut::SaveAs;
        }

        return FileMenuShortcut::None;
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

void MainMenuBar::ShowOpenProjectModal()
{
    m_openProjectError.clear();
    m_showOpenProjectModal = true;
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
    const QueueProjectOpenFn& queueProjectOpen,
    const std::function<void()>& requestClose,
    const std::function<void()>& requestNewProject,
    const std::function<void()>& requestOpenProject,
    const std::function<void()>& requestResetLayout,
    const std::function<void()>& alignSelectionToView,
    PlayModeController& playMode,
    UndoStack& undoStack,
    EditorClipboard& clipboard,
    bool allowUndoRedo)
{
    const FileMenuShortcut fileMenuShortcut = HandleFileMenuShortcuts(
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

    auto queueSaveAsModal = [&]() {
        if (m_showSaveAsModal)
        {
            return;
        }

        const std::string suggestedName = ProjectSession::SanitizeProjectName(project.GetDisplayName());
        std::snprintf(m_saveAsProjectName, sizeof(m_saveAsProjectName), "%s", suggestedName.c_str());

        std::filesystem::path parentDirectory;
        if (!project.GetProjectRootDirectory().empty())
        {
            parentDirectory = std::filesystem::path(project.GetProjectRootDirectory()).parent_path();
        }
        if (parentDirectory.empty())
        {
            parentDirectory = settings.GetLastNewProjectParentDirectory();
        }
        std::snprintf(
            m_saveAsParentDirectory,
            sizeof(m_saveAsParentDirectory),
            "%s",
            parentDirectory.string().c_str());
        m_saveAsError.clear();
        m_focusSaveAsName = true;
        m_showSaveAsModal = true;
    };

    auto queueOpenProjectModal = [&]() {
        if (requestOpenProject)
        {
            requestOpenProject();
        }
    };

    if (fileMenuShortcut == FileMenuShortcut::OpenProject)
    {
        queueOpenProjectModal();
    }
    else if (fileMenuShortcut == FileMenuShortcut::SaveAs)
    {
        queueSaveAsModal();
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
            queueOpenProjectModal();
        }

        ImGui::Separator();

        const bool canSave = !project.IsUntitled() && project.IsDirty();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, canSave))
        {
            SaveProject(scene, project, settings, editorState, captureEditorState);
        }

        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
        {
            queueSaveAsModal();
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

    // Help contains a fixed-width search field. Pin its popup to the corresponding content width
    // every frame it is open so the adjacent results popup has a stable anchor.
    const float helpMenuWidth = 260.0f + ImGui::GetStyle().WindowPadding.x * 2.0f;
    ImGui::SetNextWindowSize(ImVec2(helpMenuWidth, 0.0f), ImGuiCond_Always);
    if (ImGui::BeginMenu("Help"))
    {
        static char search[128] = {};
        ImGui::TextUnformatted("Search Renderer Tuning");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##SettingsSearch", "Type a setting...", search, sizeof(search));
        const std::string query(search);
        std::vector<const SettingRegistry::Descriptor*> searchMatches;
        if (!query.empty())
        {
            searchMatches = SettingRegistry::FindSearchMatches(query);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("About"))
        {
            ImGui::OpenPopup("About");
        }

        constexpr const char* searchPopupId = "##RendererTuningSearchResults";
        if (!query.empty())
        {
            if (!ImGui::IsPopupOpen(searchPopupId))
            {
                ImGui::OpenPopup(searchPopupId);
            }

            const ImVec2 helpMenuPos = ImGui::GetWindowPos();
            const ImVec2 helpMenuSize = ImGui::GetWindowSize();
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
            const float resultHeight = std::clamp(
                12.0f + rowHeight * static_cast<float>(std::max<std::size_t>(searchMatches.size(), 1u)),
                rowHeight + 12.0f,
                420.0f);
            ImGui::SetNextWindowPos(
                ImVec2(helpMenuPos.x + helpMenuSize.x + 4.0f, helpMenuPos.y),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(320.0f, resultHeight), ImGuiCond_Always);
            if (ImGui::BeginPopup(
                    searchPopupId,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
                        | ImGuiWindowFlags_NoFocusOnAppearing))
            {
                for (const SettingRegistry::Descriptor* entry : searchMatches)
                {
                    ImGui::PushID(entry->id.data());
                    if (ImGui::Selectable(entry->label.data()))
                    {
                        if (panels.lighting != nullptr) *panels.lighting = true;
                        TuningSectionState::RequestSearchNavigation(entry->section.data(), entry->id.data());
                        ImGui::SetWindowFocus("Renderer Tuning");
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", entry->section.data());
                    ImGui::PopID();
                }
                if (searchMatches.empty()) ImGui::TextDisabled("No matching Renderer Tuning settings.");
                ImGui::EndPopup();
            }
        }
        else if (ImGui::IsPopupOpen(searchPopupId))
        {
            if (ImGui::BeginPopup(searchPopupId))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
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

    if (m_showOpenProjectModal)
    {
        ImGui::OpenPopup("Open Project###OpenProject");
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    if (m_showOpenProjectModal && ImGui::BeginPopupModal(
            "Open Project###OpenProject",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextUnformatted("Open a recent project");
        ImGui::TextDisabled("Recent projects are stored locally by the editor.");
        ImGui::Spacing();

        auto tryOpenProject = [&](const std::string& projectPath) {
            if (playMode.IsActive())
            {
                playMode.TogglePlayStop(scene, project.GetProjectRootDirectory());
            }
            if (queueProjectOpen && queueProjectOpen(projectPath))
            {
                m_showOpenProjectModal = false;
                m_openProjectError.clear();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_openProjectError = "Could not queue the selected project for opening.";
            }
        };

        const std::vector<std::string>& recentProjects = settings.GetRecentProjects();
        if (recentProjects.empty())
        {
            ImGui::TextDisabled("No recent projects yet.");
        }
        else if (ImGui::BeginChild("Recent projects", ImVec2(0.0f, 210.0f), ImGuiChildFlags_Borders))
        {
            for (const std::string& projectPath : recentProjects)
            {
                if (projectPath == project.GetProjectFilePath())
                {
                    continue;
                }
                std::error_code existsError;
                const bool exists = std::filesystem::is_regular_file(projectPath, existsError);
                const std::string label = std::filesystem::path(projectPath).stem().string();
                ImGui::PushID(projectPath.c_str());
                if (!exists)
                {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Selectable(label.empty() ? projectPath.c_str() : label.c_str(), false, 0, ImVec2(0.0f, 0.0f)))
                {
                    tryOpenProject(projectPath);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s%s", projectPath.c_str(), exists ? "" : "\nMissing file");
                }
                if (!exists)
                {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        if (!m_openProjectError.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_openProjectError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Browse...", ImVec2(120.0f, 0.0f)))
        {
            settings.ValidateLastNewProjectParentDirectory();
            std::string projectPath;
            if (FileDialog::OpenProjectFile(projectPath, settings.GetLastNewProjectParentDirectory()))
            {
                tryOpenProject(projectPath);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_showOpenProjectModal = false;
            m_openProjectError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_showSaveAsModal)
    {
        ImGui::OpenPopup("Save New Project###SaveAsProject");
    }

    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImGui::GetMainViewport()->GetCenter(),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(
            "Save New Project###SaveAsProject",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextUnformatted("Save a new independent project copy");
        ImGui::TextWrapped(
            "A new folder is created for this project. Its Assets folder is copied first, so the original "
            "project and the new copy do not share project-relative files.");
        ImGui::Spacing();

        ImGui::TextUnformatted("Project name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_focusSaveAsName)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSaveAsName = false;
        }
        ImGui::InputText("##SaveAsProjectName", m_saveAsProjectName, sizeof(m_saveAsProjectName));

        ImGui::Spacing();
        ImGui::TextUnformatted("Parent folder");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText(
            "##SaveAsParentDirectory",
            m_saveAsParentDirectory,
            sizeof(m_saveAsParentDirectory));
        ImGui::TextDisabled("New project: <parent folder>/<project name>/<project name>.gameproject");

        if (!m_saveAsError.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_saveAsError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool submit = ImGui::Button("Save New Project", ImVec2(150.0f, 0.0f))
            || (ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive());
        if (submit)
        {
            if (captureEditorState)
            {
                captureEditorState(editorState);
            }
            if (project.DuplicateAsNewProject(
                    scene,
                    m_saveAsParentDirectory,
                    m_saveAsProjectName,
                    editorState))
            {
                RecordRecentProject(settings, project.GetProjectFilePath());
                m_showSaveAsModal = false;
                m_saveAsError.clear();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_saveAsError = project.GetStatusMessage();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_showSaveAsModal = false;
            m_saveAsError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    DrawAboutPopup();
}
