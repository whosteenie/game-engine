#pragma once

#include <imgui.h>

namespace EditorDockLayout
{
    void BuildDefaultLayout(ImGuiID dockspaceId);
    // Automation-only S0-P2 layout: Scene View and Game View occupy distinct visible dock nodes.
    void BuildDualViewportLayout(ImGuiID dockspaceId, float gameViewportFraction = 0.5f);
    void AllowViewportUndocking(ImGuiID dockspaceId);

    // Rebuild the default layout when a restored dock tree is missing after panels register.
    void ValidateRestoredLayout(ImGuiID dockspaceId);
}
