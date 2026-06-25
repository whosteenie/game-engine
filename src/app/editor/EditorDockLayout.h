#pragma once

#include <imgui.h>

namespace EditorDockLayout
{
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void AllowViewportUndocking(ImGuiID dockspaceId);

    // Returns true when a key editor panel exists but is floating outside the dock host.
    bool HasUndockedEditorPanels();

    // Re-dock floating editor panels or rebuild the default layout when the dock tree is missing.
    void RepairLayout(ImGuiID dockspaceId);
}
