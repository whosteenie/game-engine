#pragma once

#include <imgui.h>

namespace EditorDockLayout
{
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void AllowViewportUndocking(ImGuiID dockspaceId);
}
