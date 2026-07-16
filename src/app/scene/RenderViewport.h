#pragma once

#include <cstdint>

// Identifies which editor viewport is recording a frame. Scene View and Game View each
// need isolated temporal post-process state (REF-02).
enum class RenderViewport
{
    SceneView = 0,
    GameView = 1,
};

constexpr std::uint32_t RenderViewportHistoryId(const RenderViewport viewport)
{
    return static_cast<std::uint32_t>(viewport);
}
