#pragma once

struct ImGuiWindow;

// Shared contract between the scene editor and the editor-hosted viewport panels.
struct ViewportRect
{
    bool valid = false;
    bool hovered = false;

    float screenX = 0.0f;
    float screenY = 0.0f;
    float screenWidth = 0.0f;
    float screenHeight = 0.0f;

    int framebufferX = 0;
    int framebufferY = 0;
    int width = 0;
    int height = 0;

    ImGuiWindow* imguiWindow = nullptr;
};
