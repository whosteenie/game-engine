#pragma once

#include "engine/rendering/Framebuffer.h"

#include <GLFW/glfw3.h>

struct D3d12TestContext
{
    GLFWwindow* window = nullptr;
    bool gfxInitialized = false;

    bool Initialize(int width = 640, int height = 480);
    void Shutdown();
};

enum class FrameSubmitMode
{
    // Matches early tests: close/submit without swapchain present or ImGui.
    DirectSubmit,
    // Matches Application::Render: Unbind offscreen targets, EndFrame with optional ImGui.
    EditorPath,
};

void BeginOffscreenPass(Framebuffer& framebuffer, const bool bindDepthStencil = true, const bool waitForGpu = true);

void EndOffscreenPass(FrameSubmitMode mode = FrameSubmitMode::DirectSubmit);

// Finishes a frame the way SceneRenderer does: transition viewport color to SRV, then EndFrame.
void EndEditorPass(Framebuffer& framebuffer, bool compositeViewportInImGui = false);

// Presents one swapchain frame the same way Application::Render does for the project picker.
void PresentEditorSwapchainFrame();

bool ReadFramebufferPixel(const Framebuffer& framebuffer, int x, int y, float outRgba[4]);

bool ReadPresentedSwapchainPixel(int x, int y, float outRgba[4]);

constexpr float kViewportClearColor[4] = {0.08f, 0.09f, 0.15f, 1.0f};

bool IsNearClearColor(const float rgba[4], float tolerance = 0.03f);

float ColorDistanceFromClear(const float rgba[4]);

bool IsApproximatelyGrayscale(const float rgba[4], float channelTolerance = 0.06f);

// Simulates SceneRenderer's duplicate Bind() when post-process is disabled.
void BindViewportLikeSceneRenderer(Framebuffer& framebuffer);
