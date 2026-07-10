#pragma once

#include <imgui.h>

namespace EditorDockLayout
{
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void AllowViewportUndocking(ImGuiID dockspaceId);

    // Rebuild the default layout when a restored dock tree is missing after panels register.
    void ValidateRestoredLayout(ImGuiID dockspaceId);
}
