#pragma once

#include <imgui.h>

namespace EditorReorderDragDrop
{
    constexpr const char* kHierarchyDragDropPayload = "SCENE_HIERARCHY_ITEM";
    constexpr const char* kInspectorComponentDragPayload = "INSP_COMP_REORDER";

    // Invisible drop-target height; the blue insert line stays centered and thin.
    constexpr float kInsertGapHitHeight = 14.0f;

    // Delay before a collapsed hierarchy node auto-expands during drag-reparent.
    constexpr float kDragExpandDelaySeconds = 0.25f;

    inline bool IsReorderDragActive()
    {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload != nullptr
            && (payload->IsDataType(kHierarchyDragDropPayload)
                || payload->IsDataType(kInspectorComponentDragPayload));
    }
}
