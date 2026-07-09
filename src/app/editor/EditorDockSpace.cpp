#include "app/editor/EditorDockSpace.h"

#include "app/editor/EditorDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace
{
    bool HasPersistedDockLayout(ImGuiID dockspaceId)
    {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
        return node != nullptr && node->IsSplitNode();
    }
}

void EditorDockSpace::BuildLayoutIfNeeded()
{
    if (m_layoutBuilt)
    {
        return;
    }

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    if (m_forceDefaultLayout || !HasPersistedDockLayout(dockspaceId))
    {
        EditorDockLayout::BuildDefaultLayout(dockspaceId);
    }

    m_layoutBuilt = true;
    m_forceDefaultLayout = false;
}

void EditorDockSpace::Begin(const float topToolbarHeight, const bool deferLayoutBuild)
{
    m_deferLayoutBuild = deferLayoutBuild;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 hostPos = viewport->WorkPos;
    ImVec2 hostSize = viewport->WorkSize;
    hostPos.y += topToolbarHeight;
    hostSize.y = std::max(0.0f, hostSize.y - topToolbarHeight);

    ImGui::SetNextWindowPos(hostPos);
    ImGui::SetNextWindowSize(hostSize);
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
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    if (!deferLayoutBuild)
    {
        BuildLayoutIfNeeded();
    }
}

void EditorDockSpace::CommitLayout()
{
    if (!m_deferLayoutBuild)
    {
        BuildLayoutIfNeeded();
    }
}

void EditorDockSpace::End()
{
    ImGui::End();
}

void EditorDockSpace::AfterEditorPanels(const bool validateRestoredLayout)
{
    if (m_deferLayoutBuild)
    {
        m_deferLayoutBuild = false;
        BuildLayoutIfNeeded();
    }

    if (!m_layoutBuilt)
    {
        return;
    }

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    if (validateRestoredLayout)
    {
        EditorDockLayout::ValidateRestoredLayout(dockspaceId);
    }

    EditorDockLayout::AllowViewportUndocking(dockspaceId);
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
