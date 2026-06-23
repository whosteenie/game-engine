#pragma once

#include <imgui.h>

#include <cfloat>

namespace EditorPanelConstraints
{
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

        return true;
    }
}
