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
    ImGui::DockBuilderDockWindow("Renderer Tuning", dockRight);
    ImGui::DockBuilderDockWindow("Performance", dockRight);
    // Docking a tab last selects it. Keep Inspector focused on startup while retaining the
    // renderer and performance panels as adjacent tabs.
    ImGui::DockBuilderDockWindow("Inspector", dockRight);
    ImGui::DockBuilderDockWindow("Project", dockBottom);
    ImGui::DockBuilderDockWindow("Game View", dockMain);
    // Scene View is the editor's default working surface; Game View stays one tab away.
    ImGui::DockBuilderDockWindow("Scene View", dockMain);

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

void EditorDockLayout::ValidateRestoredLayout(const ImGuiID dockspaceId)
{
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockNode != nullptr && dockNode->IsSplitNode())
    {
        return;
    }

    // Dock ini is applied after editor panels register; only rebuild when the tree is still missing.
    if (ImGui::FindWindowByName("Scene View") == nullptr
        && ImGui::FindWindowByName("Hierarchy") == nullptr)
    {
        return;
    }

    BuildDefaultLayout(dockspaceId);
}
