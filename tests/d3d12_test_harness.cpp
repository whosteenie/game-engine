#include "d3d12_test_harness.h"

#include "engine/rhi/GfxContext.h"

#include <d3d12.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_glfw.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <cmath>
#include <filesystem>

bool D3d12TestContext::Initialize(const int width, const int height)
{
#ifdef _WIN32
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
    {
        std::error_code error;
        std::filesystem::current_path(std::filesystem::path(modulePath).parent_path(), error);
    }
#endif

    if (!glfwInit())
    {
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(width, height, "d3d12-render-tests", nullptr, nullptr);
    if (window == nullptr)
    {
        glfwTerminate();
        return false;
    }

    ImGui::CreateContext();
    gfxInitialized = GfxContext::Get().Initialize(window, width, height);
    if (!gfxInitialized)
    {
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }

    if (!ImGui_ImplGlfw_InitForOther(window, true))
    {
        GfxContext::Get().Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }

    return true;
}

void D3d12TestContext::Shutdown()
{
    if (gfxInitialized)
    {
        GfxContext::Get().WaitForGpuIdle();
        if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr)
        {
            ImGui_ImplDX12_Shutdown();
        }

        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui_ImplGlfw_Shutdown();
        }

        GfxContext::Get().Shutdown();
        gfxInitialized = false;
    }

    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::DestroyContext();
    }

    if (window != nullptr)
    {
        glfwDestroyWindow(window);
        window = nullptr;
    }

    glfwTerminate();
}

void FinalizeD3d12TestSession()
{
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().WaitForGpuIdle();
    }
}

void BindOffscreenTarget(Framebuffer& framebuffer, bool clearAttachments, bool bindDepthStencil);

void BeginOffscreenPass(Framebuffer& framebuffer, const bool bindDepthStencil, const bool waitForGpu)
{
    if (waitForGpu)
    {
        GfxContext::Get().WaitForGpuIdle();
    }

    GfxContext::Get().BeginFrame();
    BindOffscreenTarget(framebuffer, true, bindDepthStencil);
}

void BindOffscreenTarget(
    Framebuffer& framebuffer,
    const bool clearAttachments,
    const bool bindDepthStencil)
{
    if (bindDepthStencil)
    {
        framebuffer.BindDrawTarget(clearAttachments);
        return;
    }

    framebuffer.BindDrawTarget(clearAttachments);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    rtv.ptr = framebuffer.GetColorRtvCpuHandle(0);
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void BindViewportLikeSceneRenderer(Framebuffer& framebuffer)
{
    framebuffer.Bind();
    framebuffer.Bind();
}

void EndEditorPass(Framebuffer& framebuffer, const bool compositeViewportInImGui)
{
    framebuffer.Unbind();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (ImGuiIO* io = &ImGui::GetIO())
    {
        // Hidden GLFW windows can report a zero-sized display; force the swapchain size so
        // ImGui_ImplDX12_RenderDrawData does not early-out during offscreen tests.
        io->DisplaySize = ImVec2(
            static_cast<float>(GfxContext::Get().GetWidth()),
            static_cast<float>(GfxContext::Get().GetHeight()));
        io->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }
    ImGui::NewFrame();

    if (compositeViewportInImGui)
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(GfxContext::Get().GetWidth()),
            static_cast<float>(GfxContext::Get().GetHeight())));
        ImGui::Begin(
            "test-viewport",
            nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
        const ImTextureID textureId = static_cast<ImTextureID>(framebuffer.GetColorTexture());
        ImGui::Image(
            textureId,
            ImVec2(static_cast<float>(framebuffer.GetWidth()), static_cast<float>(framebuffer.GetHeight())));
        ImGui::End();
    }

    ImGui::Render();
    GfxContext::Get().EndFrame();
    GfxContext::Get().WaitForGpuIdle();
}

void PresentEditorSwapchainFrame()
{
    GfxContext::Get().BeginFrame();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (ImGuiIO* io = &ImGui::GetIO())
    {
        io->DisplaySize = ImVec2(
            static_cast<float>(GfxContext::Get().GetWidth()),
            static_cast<float>(GfxContext::Get().GetHeight()));
        io->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }

    ImGui::NewFrame();
    ImGui::Render();
    GfxContext::Get().EndFrame();
}

void EndOffscreenPass(const FrameSubmitMode mode)
{
    if (mode == FrameSubmitMode::EditorPath)
    {
        GfxContext::Get().EndFrame();
    }
    else
    {
        GfxContext::Get().SubmitCommandList();
    }

    GfxContext::Get().WaitForGpuIdle();
}

bool ReadFramebufferPixel(const Framebuffer& framebuffer, const int x, const int y, float outRgba[4])
{
    return framebuffer.ReadbackColorPixel(x, y, outRgba);
}

bool ReadPresentedSwapchainPixel(const int x, const int y, float outRgba[4])
{
    return GfxContext::Get().ReadbackPresentedColorPixel(x, y, outRgba);
}

bool IsNearClearColor(const float rgba[4], const float tolerance)
{
    for (int channel = 0; channel < 4; ++channel)
    {
        if (std::abs(rgba[channel] - kViewportClearColor[channel]) > tolerance)
        {
            return false;
        }
    }

    return true;
}

float ColorDistanceFromClear(const float rgba[4])
{
    float distance = 0.0f;
    for (int channel = 0; channel < 3; ++channel)
    {
        const float delta = rgba[channel] - kViewportClearColor[channel];
        distance += delta * delta;
    }

    return std::sqrt(distance);
}

bool IsApproximatelyGrayscale(const float rgba[4], const float channelTolerance)
{
    const float maxChannel = std::max({rgba[0], rgba[1], rgba[2]});
    if (maxChannel < 0.05f)
    {
        return true;
    }

    return std::abs(rgba[0] - rgba[1]) <= channelTolerance
        && std::abs(rgba[1] - rgba[2]) <= channelTolerance
        && std::abs(rgba[0] - rgba[2]) <= channelTolerance;
}
