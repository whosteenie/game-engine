#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef GetObject
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "app/core/Application.h"
#include "app/core/application/Detail.h"
#include "app/editor/EditorDockSpace.h"
#include "app/core/benchmark/Capture.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/EditorTopToolbar.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/GameViewportPanel.h"
#include "app/panels/LightingPanel.h"
#include "app/panels/PerformancePanel.h"
#include "app/editor/MainMenuBar.h"
#include "app/panels/ProjectFilesPanel.h"
#include "app/panels/SceneHierarchyPanel.h"
#include "app/panels/SceneInspectorPanel.h"
#include "app/panels/SceneToolbarPanel.h"
#include "app/panels/SceneViewportPanel.h"
#include "app/project/ProjectChooser.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "app/scene/editing/SceneEditingController.h"
#include "app/scene/editing/SceneEditor.h"
#include "app/undo/UndoStack.h"
#include "engine/assets/TextureCache.h"
#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/input/Input.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/ui/ImGuiLayer.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/rendering/core/Renderer.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Texture.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"

#include <atomic>
#include <stdexcept>
#include <string>

namespace
{
#ifdef _WIN32
    std::atomic_bool g_consoleCloseRequested{false};
    WNDPROC g_startupWindowPreviousProc = nullptr;
    HBRUSH g_startupWindowBrush = nullptr;

    LRESULT CALLBACK StartupWindowProc(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        if (message == WM_ERASEBKGND)
        {
            RECT clientRect{};
            GetClientRect(window, &clientRect);
            FillRect(reinterpret_cast<HDC>(wParam), &clientRect, g_startupWindowBrush);
            return 1;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(window, &paint);
            RECT clientRect{};
            GetClientRect(window, &clientRect);
            FillRect(deviceContext, &clientRect, g_startupWindowBrush);
            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(220, 220, 225));
            const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const HGDIOBJ previousFont = SelectObject(deviceContext, font);
            DrawTextW(
                deviceContext,
                L"Who Engine\nStarting...",
                -1,
                &clientRect,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(deviceContext, previousFont);
            EndPaint(window, &paint);
            return 0;
        }

        return g_startupWindowPreviousProc != nullptr
            ? CallWindowProcW(g_startupWindowPreviousProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    void AttachStartupWindowPaint(GLFWwindow* window)
    {
        const HWND nativeWindow = glfwGetWin32Window(window);
        g_startupWindowBrush = CreateSolidBrush(RGB(18, 18, 20));
        g_startupWindowPreviousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            nativeWindow,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(StartupWindowProc)));
        InvalidateRect(nativeWindow, nullptr, TRUE);
    }

    void DetachStartupWindowPaint(GLFWwindow* window)
    {
        if (g_startupWindowPreviousProc == nullptr)
        {
            return;
        }

        const HWND nativeWindow = glfwGetWin32Window(window);
        SetWindowLongPtrW(
            nativeWindow,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_startupWindowPreviousProc));
        g_startupWindowPreviousProc = nullptr;
        if (g_startupWindowBrush != nullptr)
        {
            DeleteObject(g_startupWindowBrush);
            g_startupWindowBrush = nullptr;
        }
    }

    BOOL WINAPI ConsoleControlHandler(const DWORD controlType)
    {
        switch (controlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            g_consoleCloseRequested.store(true, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
        }
    }

    bool ConsumeConsoleCloseRequestInternal()
    {
        return g_consoleCloseRequested.exchange(false, std::memory_order_acq_rel);
    }
#else
    bool ConsumeConsoleCloseRequestInternal()
    {
        return false;
    }
#endif

    [[noreturn]] void RethrowAsRuntimeError(const char* phase, const std::exception& exception)
    {
        throw std::runtime_error(std::string(phase) + ": " + SafeExceptionMessage(exception));
    }

}

namespace ApplicationDetail
{
    bool ConsumeConsoleCloseRequest()
    {
        return ConsumeConsoleCloseRequestInternal();
    }

    void DetachStartupWindowPaint(GLFWwindow* window)
    {
#ifdef _WIN32
        ::DetachStartupWindowPaint(window);
#else
        (void)window;
#endif
    }
}

Application::Application(int width, int height, const char* title)
    : m_width(width), m_height(height), m_title(title)
{
    InitGLFW();

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
#endif

    try
    {
        glfwPollEvents();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0)
        {
            glfwGetWindowSize(m_window, &framebufferWidth, &framebufferHeight);
        }
        framebufferWidth = std::max(framebufferWidth, 1);
        framebufferHeight = std::max(framebufferHeight, 1);

        EditorSettings::EnsureAppDataDirectoryExists();
        EngineLog::EnsureLogDirectoryExists();
        NativeProgressWindow::Instance().WarmUp();
#ifdef _WIN32
        NativeProgressWindow::Instance().SetOwnerWindow(glfwGetWin32Window(m_window));
#endif
        m_imguiLayer = std::make_unique<ImGuiLayer>(m_window, EditorSettings::GetGlobalImGuiIniPath());
        GfxContext::Get().Initialize(m_window, framebufferWidth, framebufferHeight);
        m_imguiLayer->InitPlatformBackend();

        // Persist renderer-tuning section open/close state in the editor imgui.ini. Must be
        // registered before the layout ini is loaded so saved section states are parsed.
        TuningSectionState::RegisterImGuiSettingsHandler();

        m_renderer = std::make_unique<Renderer>();
        m_editorSettings = std::make_unique<EditorSettings>();
        m_editorSettings->Load();
        GfxContext::Get().SetVsyncEnabled(m_editorSettings->IsVsyncEnabled());
        m_projectSession = std::make_unique<ProjectSession>();
        m_projectChooser = std::make_unique<ProjectChooser>();
        m_scene = std::make_unique<Scene>();
        glfwSetWindowCloseCallback(m_window, WindowCloseCallback);
        PumpStartupFramesUntilDlssReady();

        UpdatePendingProjectStartupProgress("Preparing editor...");
        m_mainMenuBar = std::make_unique<MainMenuBar>();
        glfwPollEvents();
        m_editorTopToolbar = std::make_unique<EditorTopToolbar>();
        UpdatePendingProjectStartupProgress("Preparing editor panels...");
        m_lightingPanel = std::make_unique<LightingPanel>();
        glfwPollEvents();
        m_performancePanel = std::make_unique<PerformancePanel>();
        m_sceneToolbarPanel = std::make_unique<SceneToolbarPanel>();
        m_sceneHierarchyPanel = std::make_unique<SceneHierarchyPanel>();
        m_sceneInspectorPanel = std::make_unique<SceneInspectorPanel>();
        m_projectFilesPanel = std::make_unique<ProjectFilesPanel>();
        m_sceneViewportPanel = std::make_unique<SceneViewportPanel>();
        m_gameViewportPanel = std::make_unique<GameViewportPanel>();
        m_editorDockSpace = std::make_unique<EditorDockSpace>();
        glfwPollEvents();
        UpdatePendingProjectStartupProgress("Preparing editor viewports...");
        m_camera = std::make_unique<Camera>(
            glm::vec3(6.0f, 5.0f, 6.0f),
            -135.0f,
            -35.0f);

        OnFramebufferResize(framebufferWidth, framebufferHeight);

        m_input = std::make_unique<Input>(m_window);
        m_sceneEditingController = std::make_unique<SceneEditingController>();
        m_scene->BindSceneEditor(m_sceneEditingController->GetEditor());
        m_playModeController.SetSceneEditor(m_sceneEditingController->GetEditor());
        m_scene->SetDirtyCallback([this]() { m_projectSession->MarkDirty(); });

        ProcessQueuedProjectOpenIfReady();
    }
    catch (...)
    {
#ifdef _WIN32
        DetachStartupWindowPaint(m_window);
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
#endif

        if (m_window != nullptr)
        {
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }

        glfwTerminate();
        throw;
    }
}

Application::~Application()
{
#ifdef _WIN32
    DetachStartupWindowPaint(m_window);
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
#endif

    EditorSettings::SaveEditorLayout(
        m_projectSession != nullptr && m_projectSession->HasActiveProject()
            ? m_projectSession->GetProjectRootDirectory()
            : std::string{});

    if (m_editorSettings)
    {
        m_editorSettings->Save();
    }

    NativeProgressWindow::Instance().Shutdown();

    // Release all D3D12MA-backed resources before the global allocator shuts down.
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().PrepareForDeviceShutdown();

        if (m_playModeController.IsActive() && m_scene != nullptr && m_projectSession != nullptr
            && m_projectSession->HasActiveProject())
        {
            m_playModeController.TogglePlayStop(*m_scene, m_projectSession->GetProjectRootDirectory());
        }

        m_undoStack.Clear();
        m_playModeDiscardUndoStack.Clear();
        m_editorClipboard.Clear();

        m_gameViewportPanel.reset();
        m_sceneViewportPanel.reset();
        m_sceneEditingController.reset();
        m_scene.reset();
        m_renderer.reset();
        TextureCache::Get().Clear();
        RenderingPipelineCache::InvalidateAll();
        Material::ReleaseGlobalGpuResources();
        Texture::ReleaseUploadResources();
        GfxContext::Get().PrepareForDeviceShutdown();
    }

    m_imguiLayer.reset();
    GfxContext::Get().Shutdown();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}


void Application::InitGLFW()
{
    if (!glfwInit())
    {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(
        GLFW_MAXIMIZED,
        std::getenv("GAME_ENGINE_S2P4_WINDOWED") != nullptr
                || std::getenv("GAME_ENGINE_S2P5_MODE_MATRIX") != nullptr
            ? GLFW_FALSE
            : GLFW_TRUE);

    m_window = glfwCreateWindow(m_width, m_height, m_title, nullptr, nullptr);
    if (!m_window)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, FramebufferSizeCallback);
#ifdef _WIN32
    AttachStartupWindowPaint(m_window);
#endif
    glfwShowWindow(m_window);
#ifdef _WIN32
    UpdateWindow(glfwGetWin32Window(m_window));
#endif

#ifdef _WIN32
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
    {
        std::error_code error;
        std::filesystem::current_path(std::filesystem::path(modulePath).parent_path(), error);
    }
#endif
}

void Application::WindowCloseCallback(GLFWwindow* window)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    app->RequestClose();
}

void Application::OnFramebufferResize(int width, int height)
{
    m_renderer->SetViewport(width, height);
}

void Application::FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->OnFramebufferResize(width, height);
}


