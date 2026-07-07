#pragma once

namespace TuningSectionState
{
    // Registers an ImGui settings handler so the renderer-tuning panel's top-level section
    // (CollapsingHeader) open/close state is persisted inside the editor imgui.ini, alongside the
    // window/dock layout. Call once, after the ImGui context exists and before the editor layout ini
    // is loaded. Idempotent.
    void RegisterImGuiSettingsHandler();

    // Drop-in replacement for ImGui::CollapsingHeader for the tuning panel's top-level sections.
    // Seeds the header from persisted (or, on first ever use, default) state, then tracks user
    // toggles and marks the ini dirty so the state saves with the rest of the editor UI. Returns
    // whether the section is open (same semantics as ImGui::CollapsingHeader).
    bool SectionHeader(const char* label, bool defaultOpen);
}
