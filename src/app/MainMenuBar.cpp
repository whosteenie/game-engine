#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
            ImGui::TextUnformatted("Project file management is scaffolded.");
            ImGui::TextUnformatted("Save/load and preferences are not wired yet.");
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

        const std::vector<int> importedIndices = scene.ImportModel(modelPath, parentIndex);
        if (importedIndices.empty())
        {
            return;
        }

        scene.SetSelectedObjectIndex(importedIndices.front());
        project.MarkDirty();
    }
}

void MainMenuBar::Draw(
    Scene& scene,
    ProjectSession& project,
    GLFWwindow* window,
    const EditorPanelVisibility& panels)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
        {
            std::string folderPath;
            if (FileDialog::ChooseProjectFolder(folderPath))
            {
                project.NewAt(folderPath);
            }
        }

        if (ImGui::MenuItem("Open Project...", "Ctrl+O"))
        {
            std::string projectPath;
            if (FileDialog::OpenProjectFile(projectPath))
            {
                project.Load(projectPath);
            }
        }

        ImGui::Separator();

        const bool canSaveInPlace = !project.IsUntitled();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, canSaveInPlace))
        {
            if (!project.Save())
            {
                std::string projectPath;
                if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
                {
                    project.SaveAs(projectPath);
                }
            }
        }

        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
        {
            std::string projectPath;
            if (FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
            {
                project.SaveAs(projectPath);
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Import Model..."))
        {
            ImportModelIntoScene(scene, project, -1);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Exit", "Alt+F4"))
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
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
