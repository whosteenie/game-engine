#include "engine/platform/ImGuiLayer.h"

#include "engine/platform/ImGuiFonts.h"
#include "engine/rhi/GfxContext.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_glfw.h>

#include <ImGuizmo.h>

#include <imgui_internal.h>

#include <stdexcept>

ImGuiLayer::ImGuiLayer(GLFWwindow* window, const std::string& iniPath)
    : m_window(window), m_iniPath(iniPath)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingNoSplit = false;
    io.ConfigDockingNoDockingOver = false;
    io.ConfigErrorRecovery = true;
    // Defer imgui.ini load until a project is opened. Loading a saved editor dock
    // layout on the project picker leaves invisible host windows that steal input.
    io.IniFilename = nullptr;

    ImGuiFonts::LoadEditorFonts(io);
}

void ImGuiLayer::InitPlatformBackend()
{
    if (m_window == nullptr)
    {
        throw std::runtime_error("ImGuiLayer has no GLFW window");
    }

    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
    {
        return;
    }

    if (!ImGui_ImplGlfw_InitForOther(m_window, true))
    {
        throw std::runtime_error("Failed to initialize ImGui GLFW backend");
    }
}

ImGuiLayer::~ImGuiLayer()
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().WaitForGpuIdle();
    }

    // DX12 must shut down before GLFW: ImGui_ImplDX12_Init always sets main viewport
    // RendererUserData, but Renderer_DestroyWindow is only registered with ViewportsEnable.
    if (ImGui::GetIO().BackendRendererUserData != nullptr)
    {
        ImGui_ImplDX12_Shutdown();
    }

    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
    {
        ImGui_ImplGlfw_Shutdown();
    }

    ImGui::DestroyContext();
}

void ImGuiLayer::BeginFrame()
{
    ImGui_ImplGlfw_NewFrame();
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void ImGuiLayer::EndFrame()
{
    ImGui::Render();
}

void ImGuiLayer::CancelInterruptedFrame()
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr || !context->WithinFrameScope)
    {
        return;
    }

    // Best-effort stack drain so EndFrame does not assert on Missing End().
    constexpr int kMaxRecoverySteps = 256;
    for (int step = 0; step < kMaxRecoverySteps && context->CurrentWindow != nullptr; ++step)
    {
        ImGuiWindow* window = context->CurrentWindow;
        if ((window->Flags & ImGuiWindowFlags_Popup) != 0)
        {
            ImGui::EndPopup();
        }
        else if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0)
        {
            ImGui::EndChild();
        }
        else
        {
            ImGui::End();
        }
    }

    if (context->WithinFrameScope && context->WithinFrameScopeWithImplicitWindow)
    {
        ImGui::End();
    }

    if (context->WithinFrameScope)
    {
        ImGui::EndFrame();
    }
}
