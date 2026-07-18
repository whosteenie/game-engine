#pragma once

struct ImGuiIO;

namespace ImGuiFonts
{
    // Load the primary UI font and merge icon glyphs into the same atlas (no PushFont/PopFont per icon).
    // Call after ImGui::CreateContext() and before the ImGui backend Init().
    void LoadEditorFonts(ImGuiIO& io);

    bool IconsAvailable();
}
