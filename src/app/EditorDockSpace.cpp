#include "app/EditorDockSpace.h"

#include "app/EditorDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
    bool HasPersistedDockLayout(ImGuiID dockspaceId)
    {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
        return node != nullptr && node->IsSplitNode();
    }
}

void EditorDockSpace::Begin()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##EditorDockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    constexpr ImGuiDockNodeFlags kDockSpaceFlags = ImGuiDockNodeFlags_NoDockingOverCentralNode;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), kDockSpaceFlags);

    if (!m_layoutBuilt)
    {
        if (m_forceDefaultLayout || !HasPersistedDockLayout(dockspaceId))
        {
            EditorDockLayout::BuildDefaultLayout(dockspaceId);
        }

        m_layoutBuilt = true;
        m_forceDefaultLayout = false;
    }
}

void EditorDockSpace::End()
{
    ImGui::End();
}

namespace ImGui
{
void ClearIniSettings();
}

void EditorDockSpace::ResetLayout()
{
    ImGui::ClearIniSettings();
    RequestLayoutRebuild();
}
