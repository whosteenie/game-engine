#pragma once

#include "app/editor/OffscreenViewportPanel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>

namespace EditorPanelConstraints
{
    inline bool IsSelectedDockTab()
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window == nullptr || !window->DockIsActive || window->DockNode == nullptr)
        {
            return true;
        }

        return window->DockNode->SelectedTabId == window->TabId;
    }

    inline bool IsViewportTabSelected(const char* windowName)
    {
        ImGuiWindow* window = ImGui::FindWindowByName(windowName);
        if (window == nullptr)
        {
            return false;
        }

        if (!window->DockIsActive || window->DockNode == nullptr)
        {
            return !window->Collapsed;
        }

        return window->DockNode->SelectedTabId == window->TabId;
    }

    inline void SyncViewportDockVisibleWindow(const char* firstWindowName, const char* secondWindowName)
    {
        ImGuiWindow* firstWindow = ImGui::FindWindowByName(firstWindowName);
        ImGuiWindow* secondWindow = ImGui::FindWindowByName(secondWindowName);
        if (firstWindow == nullptr
            || secondWindow == nullptr
            || firstWindow->DockNode == nullptr
            || firstWindow->DockNode != secondWindow->DockNode)
        {
            return;
        }

        ImGuiDockNode* node = firstWindow->DockNode;
        if (node->SelectedTabId == secondWindow->TabId)
        {
            node->VisibleWindow = secondWindow;
        }
        else if (node->SelectedTabId == firstWindow->TabId)
        {
            node->VisibleWindow = firstWindow;
        }
    }

    inline void ApplySideColumnPanel()
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 160.0f), ImVec2(FLT_MAX, FLT_MAX));
    }

    inline void ApplySceneViewPanel()
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f), ImVec2(FLT_MAX, FLT_MAX));
    }

    inline void ApplyProjectPanel()
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 140.0f), ImVec2(FLT_MAX, FLT_MAX));
    }

    inline bool BeginDockedPanel(const char* name, bool& showPanel, ImGuiWindowFlags flags = ImGuiWindowFlags_None)
    {
        if (!showPanel)
        {
            return false;
        }

        if (!ImGui::Begin(name, &showPanel, flags))
        {
            ImGui::End();
            return false;
        }

        if (!IsSelectedDockTab())
        {
            const ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
            const ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
            const ImVec2 windowPos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                windowPos + contentMin,
                windowPos + contentMax,
                OffscreenViewportPanel::kBackgroundColor);
            ImGui::End();
            return false;
        }

        return true;
    }
}
