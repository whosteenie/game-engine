#include "app/editor/EditorMouseWrapping.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
    ImGuiID g_wrapEligibleActiveId = 0;
}

namespace EditorMouseWrapping
{
    void MarkCurrentItemForMouseWrap()
    {
        ImGuiContext* context = ImGui::GetCurrentContext();
        if (context == nullptr || !ImGui::IsItemActive())
        {
            return;
        }

        g_wrapEligibleActiveId = context->LastItemData.ID;
    }

    bool IsActiveItemMouseWrapEligible()
    {
        ImGuiContext* context = ImGui::GetCurrentContext();
        return context != nullptr
            && context->ActiveId != 0
            && context->ActiveId == g_wrapEligibleActiveId;
    }
}
