#pragma once

// Identifies which editor viewport is recording a frame. Scene View and Game View each
// need isolated temporal post-process state (REF-02).
enum class RenderViewport
{
    SceneView,
    GameView,
};
