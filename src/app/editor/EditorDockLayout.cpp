#include "app/editor/EditorDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
    constexpr float kLeftColumnFraction = 0.22f;
    constexpr float kRightColumnFraction = 0.20f;
    constexpr float kBottomRowFraction = 0.20f;
}

void EditorDockLayout::BuildDefaultLayout(ImGuiID dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID dockMain = dockspaceId;
    const ImGuiID dockLeft =
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, kLeftColumnFraction, nullptr, &dockMain);
    const ImGuiID dockRight =
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, kRightColumnFraction, nullptr, &dockMain);
    const ImGuiID dockBottom =
        ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, kBottomRowFraction, nullptr, &dockMain);

    ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
    ImGui::DockBuilderDockWindow("Inspector", dockRight);
    ImGui::DockBuilderDockWindow("Renderer Tuning", dockRight);
    ImGui::DockBuilderDockWindow("Project", dockBottom);
    ImGui::DockBuilderDockWindow("Scene View", dockMain);
    ImGui::DockBuilderDockWindow("Game View", dockMain);

    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockLayout::AllowViewportUndocking(const ImGuiID dockspaceId)
{
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId))
    {
        centralNode->SetLocalFlags(
            centralNode->LocalFlags & ~ImGuiDockNodeFlags_NoUndocking);
    }
}

namespace
{
    bool IsEditorPanelFloating(const char* windowName)
    {
        ImGuiWindow* window = ImGui::FindWindowByName(windowName);
        if (window == nullptr)
        {
            return false;
        }

        return !window->DockIsActive;
    }
}

bool EditorDockLayout::HasUndockedEditorPanels()
{
    static constexpr const char* kEditorPanelNames[] = {
        "Game View",
        "Scene View",
        "Renderer Tuning",
        "Inspector",
        "Hierarchy",
        "Project",
    };

    for (const char* panelName : kEditorPanelNames)
    {
        if (IsEditorPanelFloating(panelName))
        {
            return true;
        }
    }

    return false;
}

void EditorDockLayout::RepairLayout(const ImGuiID dockspaceId)
{
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockNode == nullptr || !dockNode->IsSplitNode())
    {
        // Dock ini may not be applied until editor panels register on the first frame.
        if (ImGui::FindWindowByName("Scene View") == nullptr
            && ImGui::FindWindowByName("Hierarchy") == nullptr)
        {
            return;
        }

        BuildDefaultLayout(dockspaceId);
        return;
    }

    if (!HasUndockedEditorPanels())
    {
        return;
    }

    BuildDefaultLayout(dockspaceId);
}
