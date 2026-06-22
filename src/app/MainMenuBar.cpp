#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/EditorSettings.h"
#include "app/MainMenuBar.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"
#include "engine/FileDialog.h"

#include <imgui.h>

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

    void ImportModelIntoScene(Scene& scene, ProjectSession& project, int parentIndex)
    {
        std::string modelPath;
        if (!FileDialog::OpenModelFile(modelPath))
        {
            return;
        }

        const std::vector<int> importedIndices = scene.ImportModel(
            modelPath,
            parentIndex,
            project.GetProjectRootDirectory());
        if (importedIndices.empty())
        {
            if (!scene.GetLastImportError().empty())
            {
                project.SetStatusMessage(scene.GetLastImportError());
            }
            return;
        }

        scene.SetSelectedObjectIndex(importedIndices.front());
        if (!scene.GetLastImportWarning().empty())
        {
            project.SetStatusMessage(scene.GetLastImportWarning());
        }
    }

    void OpenProject(Scene& scene, ProjectSession& project, EditorSettings& settings)
    {
        std::string projectPath;
        if (!FileDialog::OpenProjectFile(projectPath))
        {
            return;
        }

        if (project.OpenProject(scene, projectPath))
        {
            settings.AddRecentProject(projectPath);
            settings.Save();
        }
    }

    void SaveProject(Scene& scene, ProjectSession& project)
    {
        if (project.IsUntitled() || !project.IsDirty())
        {
            return;
        }

        if (!project.Save(scene))
        {
            std::string projectPath;
            if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
            {
                project.SaveAs(scene, projectPath);
            }
        }
    }

    void SaveProjectAs(Scene& scene, ProjectSession& project)
    {
        std::string projectPath;
        if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
        {
            project.SaveAs(scene, projectPath);
        }
    }

    bool AllowFileMenuShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        return !io.WantTextInput && !ImGui::IsAnyItemActive();
    }

    void HandleFileMenuShortcuts(Scene& scene, ProjectSession& project, EditorSettings& settings)
    {
        if (!AllowFileMenuShortcuts())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
            OpenProject(scene, project, settings);
        }

        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveProject(scene, project);
        }

        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveProjectAs(scene, project);
        }
    }
}

void MainMenuBar::Draw(
    Scene& scene,
    ProjectSession& project,
    EditorSettings& settings,
    GLFWwindow* window,
    const EditorPanelVisibility& panels,
    const std::function<void()>& requestClose,
    const std::function<void()>& requestNewProject)
{
    HandleFileMenuShortcuts(scene, project, settings);

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
            OpenProject(scene, project, settings);
        }

        ImGui::Separator();

        const bool canSave = !project.IsUntitled() && project.IsDirty();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, canSave))
        {
            SaveProject(scene, project);
        }

        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
        {
            SaveProjectAs(scene, project);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Import Model..."))
        {
            ImportModelIntoScene(scene, project, -1);
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
        ImGui::BeginDisabled();
        ImGui::MenuItem("Undo", "Ctrl+Z");
        ImGui::MenuItem("Redo", "Ctrl+Y");
        ImGui::EndDisabled();

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
