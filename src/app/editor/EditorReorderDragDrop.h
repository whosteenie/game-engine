#pragma once

namespace EditorReorderDragDrop
{
    // Invisible drop-target height; the blue insert line stays centered and thin.
    constexpr float kInsertGapHitHeight = 14.0f;

    // Delay before a collapsed hierarchy node auto-expands during drag-reparent.
    constexpr float kDragExpandDelaySeconds = 0.25f;
}
