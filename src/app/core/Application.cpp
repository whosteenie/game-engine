#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/core/Application.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/EditorDockSpace.h"
#include "app/editor/EditorMouseWrapping.h"
#include "app/editor/EditorTopToolbar.h"
#include "app/editor/EditorViewportRect.h"
#include "app/panels/GameViewportPanel.h"
#include "app/panels/LightingPanel.h"
#include "app/panels/PerformancePanel.h"
#include "app/editor/MainMenuBar.h"
#include "app/editor/EditorReorderDragDrop.h"
#include "app/project/ProjectChooser.h"
#include "app/project/ProjectEditorState.h"
#include "app/panels/ProjectFilesPanel.h"
#include "app/project/ProjectSession.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneEditingController.h"
#include "app/scene/SceneEditorUpdateContext.h"
#include "app/panels/SceneHierarchyPanel.h"
#include "app/panels/SceneInspectorPanel.h"
#include "app/scene/SceneCamera.h"
#include "app/panels/SceneToolbarPanel.h"
#include "app/panels/SceneViewportPanel.h"
#include "app/undo/UndoContext.h"
#include "app/undo/UndoStack.h"
#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/assets/FileDialog.h"
#include "engine/assets/TextureCache.h"
#include "engine/platform/ImGuiLayer.h"
#include "engine/platform/EngineLog.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "engine/rhi/GfxContext.h"

#include <ImGuizmo.h>
#include "engine/platform/NativeProgressWindow.h"
#include "engine/platform/Input.h"
#include "engine/platform/InputDiagnostics.h"
#include "engine/platform/FrameDiagnostics.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/rendering/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <algorithm>
#include <stdexcept>

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <typeinfo>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef GetObject
#endif
#include <glm/gtc/matrix_inverse.hpp>
#include <unordered_map>

namespace
{
    [[noreturn]] void RethrowAsRuntimeError(const char* phase, const std::exception& exception)
    {
        throw std::runtime_error(std::string(phase) + ": " + SafeExceptionMessage(exception));
    }

    std::string DescribeException(const std::exception& exception)
    {
        return SafeExceptionMessage(exception);
    }

    template<typename Fn>
    void RunApplicationPhase(const char* phase, Fn&& fn)
    {
        try
        {
            fn();
        }
        catch (const std::exception& exception)
        {
            const std::string safeMessage = SafeExceptionMessage(exception);
            EngineLog::LogFailure("application", phase, safeMessage);
            throw std::runtime_error(std::string(phase) + ": " + safeMessage);
        }
    }

    bool AlignPrimarySelectionToCameraView(Scene& scene, const Camera& camera, UndoStack* undoStack)
    {
        const int selectedIndex = scene.GetPrimarySelection();
        if (selectedIndex < 0
            || static_cast<std::size_t>(selectedIndex) >= scene.GetObjects().size())
        {
            return false;
        }

        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(selectedIndex));
        const glm::mat4 cameraWorldMatrix = glm::affineInverse(camera.GetViewMatrix());
        glm::mat4 localMatrix = cameraWorldMatrix;
        if (object.GetParentIndex() >= 0)
        {
            const glm::mat4 parentWorldMatrix = scene.GetWorldMatrix(object.GetParentIndex());
            localMatrix = glm::affineInverse(parentWorldMatrix) * cameraWorldMatrix;
        }

        ObjectTransformMap before;
        if (undoStack != nullptr)
        {
            before = CaptureLocalTransforms(scene, {selectedIndex});
        }

        Transform& transform = object.GetTransform();
        const glm::vec3 preservedScale = transform.scale;
        transform.SetFromMatrix(localMatrix);
        transform.scale = preservedScale;
        scene.MarkDirty();

        if (undoStack != nullptr)
        {
            ObjectTransformMap after = CaptureLocalTransforms(scene, {selectedIndex});
            PushTransformObjects(*undoStack, std::move(before), std::move(after), "Align to View");
        }

        return true;
    }

    bool IsPointInEditorViewportRect(const EditorViewportRect& rect, const double x, const double y)
    {
        if (!rect.valid || rect.screenWidth <= 0.0f || rect.screenHeight <= 0.0f)
        {
            return false;
        }

        return x >= static_cast<double>(rect.screenX)
            && x < static_cast<double>(rect.screenX + rect.screenWidth)
            && y >= static_cast<double>(rect.screenY)
            && y < static_cast<double>(rect.screenY + rect.screenHeight);
    }

    void SuppressImGuiMouseInput()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseWheel = 0.0f;
        io.MouseWheelH = 0.0f;
        std::memset(io.MouseDown, 0, sizeof(io.MouseDown));
        io.WantCaptureMouse = false;
    }

    bool ShouldWrapMouseForImGuiInfiniteDrag()
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();

        if (!ImGui::IsAnyItemActive())
        {
            return false;
        }

        if (ImGuizmo::IsUsing() || ImGuizmo::IsUsingViewManipulate())
        {
            return false;
        }

        if (g.MovingWindow != nullptr)
        {
            return false;
        }

        if (g.DragDropActive)
        {
            return false;
        }

        if (EditorReorderDragDrop::IsReorderDragActive())
        {
            return false;
        }

        const int horizontalNavMask = (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        if ((g.ActiveIdUsingNavDirMask & horizontalNavMask) != horizontalNavMask)
        {
            return false;
        }

        return EditorMouseWrapping::IsActiveItemMouseWrapEligible()
            && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    }

    void WrapImGuiMouseCursorAtWindowEdges(GLFWwindow* window)
    {
        if (!ShouldWrapMouseForImGuiInfiniteDrag())
        {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 displaySize = io.DisplaySize;
        if (displaySize.x <= 1.0f || displaySize.y <= 1.0f)
        {
            return;
        }

        double cursorX = 0.0;
        double cursorY = 0.0;
        glfwGetCursorPos(window, &cursorX, &cursorY);

        constexpr double margin = 1.0;
        double newCursorX = cursorX;
        double newCursorY = cursorY;
        bool wrapped = false;

        if (cursorX <= margin)
        {
            newCursorX = static_cast<double>(displaySize.x) - margin - 1.0;
            wrapped = true;
        }
        else if (cursorX >= static_cast<double>(displaySize.x) - margin - 1.0)
        {
            newCursorX = margin + 1.0;
            wrapped = true;
        }

        if (cursorY <= margin)
        {
            newCursorY = static_cast<double>(displaySize.y) - margin - 1.0;
            wrapped = true;
        }
        else if (cursorY >= static_cast<double>(displaySize.y) - margin - 1.0)
        {
            newCursorY = margin + 1.0;
            wrapped = true;
        }

        if (!wrapped)
        {
            return;
        }

        const ImVec2 wrapOffset(
            static_cast<float>(newCursorX - cursorX),
            static_cast<float>(newCursorY - cursorY));

        glfwSetCursorPos(window, newCursorX, newCursorY);
        io.MousePos.x += wrapOffset.x;
        io.MousePos.y += wrapOffset.y;

        for (int button = 0; button < IM_ARRAYSIZE(io.MouseClickedPos); ++button)
        {
            if (!ImGui::IsMouseDown(button))
            {
                continue;
            }

            io.MouseClickedPos[button].x += wrapOffset.x;
            io.MouseClickedPos[button].y += wrapOffset.y;
        }
    }
}

Application::Application(int width, int height, const char* title)
    : m_width(width), m_height(height), m_title(title)
{
    InitGLFW();

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
        m_imguiLayer = std::make_unique<ImGuiLayer>(m_window, EditorSettings::GetGlobalImGuiIniPath());
        GfxContext::Get().Initialize(m_window, framebufferWidth, framebufferHeight);
        m_imguiLayer->InitPlatformBackend();

        m_renderer = std::make_unique<Renderer>();
        m_editorSettings = std::make_unique<EditorSettings>();
        m_editorSettings->Load();
        m_projectSession = std::make_unique<ProjectSession>();
        m_projectChooser = std::make_unique<ProjectChooser>();
        m_mainMenuBar = std::make_unique<MainMenuBar>();
        m_editorTopToolbar = std::make_unique<EditorTopToolbar>();
        m_lightingPanel = std::make_unique<LightingPanel>();
        m_performancePanel = std::make_unique<PerformancePanel>();
        m_sceneToolbarPanel = std::make_unique<SceneToolbarPanel>();
        m_sceneHierarchyPanel = std::make_unique<SceneHierarchyPanel>();
        m_sceneInspectorPanel = std::make_unique<SceneInspectorPanel>();
        m_projectFilesPanel = std::make_unique<ProjectFilesPanel>();
        m_sceneViewportPanel = std::make_unique<SceneViewportPanel>();
        m_gameViewportPanel = std::make_unique<GameViewportPanel>();
        m_editorDockSpace = std::make_unique<EditorDockSpace>();
        m_camera = std::make_unique<Camera>(
            glm::vec3(6.0f, 5.0f, 6.0f),
            -135.0f,
            -35.0f);

        OnFramebufferResize(framebufferWidth, framebufferHeight);

        m_input = std::make_unique<Input>(m_window);
        m_scene = std::make_unique<Scene>();
        m_sceneEditingController = std::make_unique<SceneEditingController>();
        m_scene->BindSceneEditor(m_sceneEditingController->GetEditor());
        m_playModeController.SetSceneEditor(m_sceneEditingController->GetEditor());
        m_scene->SetDirtyCallback([this]() { m_projectSession->MarkDirty(); });

        glfwSetWindowCloseCallback(m_window, WindowCloseCallback);
    }
    catch (...)
    {
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
    EditorSettings::SaveGlobalEditorLayout();

    if (m_editorSettings)
    {
        m_editorSettings->Save();
    }

    NativeProgressWindow::Instance().Shutdown();

    // Release all D3D12MA-backed resources before the global allocator shuts down.
    if (GfxContext::Get().IsInitialized())
    {
        GfxContext::Get().WaitForGpuIdle();

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
        ShaderCache::Clear();
        Material::ReleaseGlobalGpuResources();
        Texture::ReleaseUploadResources();
    }

    m_imguiLayer.reset();
    GfxContext::Get().Shutdown();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Application::Run()
{
    if (const char* autoOpenPath = std::getenv("GAME_ENGINE_AUTO_OPEN"))
    {
        std::string error;
        if (std::getenv("GAME_ENGINE_AUTO_OPEN_DEFERRED") != nullptr)
        {
            m_projectChooser->QueueProjectOpen(autoOpenPath);
        }
        else if (!m_projectChooser->OpenProjectAtPath(
                     *m_projectSession,
                     *m_scene,
                     *m_editorSettings,
                     m_projectEditorState,
                     autoOpenPath,
                     [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
                     m_undoStack,
                     m_editorClipboard,
                     [this]() { EnsureEditorLayoutLoaded(); },
                     error))
        {
            m_projectChooser->SetErrorMessage(error.empty() ? "Failed to open project." : error);
        }
    }

    double lastFrameTime = glfwGetTime();
    std::string lastLoggedFrameError;
    int suppressedRepeatedFrameErrors = 0;

    while (!glfwWindowShouldClose(m_window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        try
        {
            RunApplicationPhase("Update", [&]() { Update(deltaTime); });
            RunApplicationPhase("Render", [&]() { Render(); });
            suppressedRepeatedFrameErrors = 0;
        }
        catch (const std::exception& exception)
        {
            const std::string described = DescribeException(exception);
            const std::string message = "Application frame: " + described;
            InputDiagnostics::Log(message.c_str());
            if (described.find("device removed") != std::string::npos
                || described.find("Present failed") != std::string::npos
                || described.find("HRESULT=0x887a") != std::string::npos)
            {
                EngineLog::Error("application", "Fatal GPU error — closing application.");
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            if (described != lastLoggedFrameError)
            {
                lastLoggedFrameError = described;
                suppressedRepeatedFrameErrors = 0;
                std::cerr << message << "\n";
                if (m_projectSession != nullptr)
                {
                    m_projectSession->SetStatusMessage(message);
                }
            }
            else if (suppressedRepeatedFrameErrors < 3)
            {
                ++suppressedRepeatedFrameErrors;
                std::cerr << message << "\n";
            }
            else if (suppressedRepeatedFrameErrors == 3)
            {
                ++suppressedRepeatedFrameErrors;
                std::cerr << "Application frame: (suppressing repeated errors)\n";
                if (m_projectSession != nullptr)
                {
                    m_projectSession->SetStatusMessage("Application frame: " + described);
                }
            }
            RecoverInterruptedFrame();
        }
        catch (...)
        {
            EngineLog::Error("application", "Application frame: unknown exception");
            std::cerr << "Application frame: unknown exception\n";
            if (m_projectSession != nullptr)
            {
                m_projectSession->SetStatusMessage("Application frame: unknown exception");
            }
            RecoverInterruptedFrame();
        }
    }
}

void Application::RecoverInterruptedFrame()
{
    if (m_gfxFrameActive)
    {
        try
        {
            m_renderer->CancelFrame();
        }
        catch (...)
        {
        }

        m_gfxFrameActive = false;
    }

    if (m_imguiFrameActive)
    {
        try
        {
            m_imguiLayer->CancelInterruptedFrame();
        }
        catch (...)
        {
        }

        m_imguiFrameActive = false;
    }
}

void Application::InitGLFW()
{
    if (!glfwInit())
    {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    m_window = glfwCreateWindow(m_width, m_height, m_title, nullptr, nullptr);
    if (!m_window)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, FramebufferSizeCallback);
    glfwShowWindow(m_window);

#ifdef _WIN32
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
    {
        std::error_code error;
        std::filesystem::current_path(std::filesystem::path(modulePath).parent_path(), error);
    }
#endif
}

void Application::InitGLAD()
{
    // OpenGL path removed on D3D12 branch; kept for API compatibility.
}

void Application::Update(double deltaTime)
{
    m_performancePanel->OnFrame(deltaTime);

    glfwPollEvents();
    InputDiagnostics::LogFrame(m_window, "after-poll");

    std::string pendingProjectError;
    const bool openedProject = m_projectChooser->ProcessPendingProjectOpen(
        *m_projectSession,
        *m_scene,
        *m_editorSettings,
        m_projectEditorState,
        [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
        m_undoStack,
        m_editorClipboard,
        [this]() { EnsureEditorLayoutLoaded(); },
        pendingProjectError);
    if (!openedProject && !pendingProjectError.empty())
    {
        m_projectChooser->SetErrorMessage(pendingProjectError);
    }

    const bool escapePressed = m_input->WasKeyPressed(GLFW_KEY_ESCAPE);
    const bool cancelReorderDragOnly =
        escapePressed && EditorReorderDragDrop::IsReorderDragActive();

    m_imguiLayer->BeginFrame();
    m_imguiFrameActive = true;
    InputDiagnostics::LogFrame(m_window, "after-imgui-newframe");

    m_projectChooser->Draw(
        *m_projectSession,
        *m_scene,
        *m_editorSettings,
        m_projectEditorState,
        [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
        [this]() { RequestClose(); },
        m_undoStack,
        m_editorClipboard);

    InputDiagnostics::LogFrame(m_window, "after-ui-build");

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();

    const bool blockSceneInputEarly = m_pendingClose || m_pendingNewProject;
    if (editorActive)
    {
        const EditorViewportRect& sceneViewRect = m_sceneViewportPanel->GetInteractionRect();
        double cursorX = 0.0;
        double cursorY = 0.0;
        m_input->GetCursorPosition(cursorX, cursorY);
        const bool mouseOverSceneView = IsPointInEditorViewportRect(sceneViewRect, cursorX, cursorY);
        const bool gameViewBlocksSceneInput =
            m_playModeController.IsActive() && m_gameViewportPanel->GetInteractionRect().hovered;
        const bool allowFlyCameraCapture =
            m_sceneViewportPanel->HasValidRenderTarget()
            && mouseOverSceneView
            && !gameViewBlocksSceneInput
            && !blockSceneInputEarly;

        m_input->UpdateMouseCapture(allowFlyCameraCapture);
        if (m_input->IsCapturingMouse())
        {
            SuppressImGuiMouseInput();
        }
    }

    if (!m_input->IsCapturingMouse())
    {
        WrapImGuiMouseCursorAtWindowEdges(m_window);
    }

    if (editorActive)
    {
        int windowWidth = 0;
        int windowHeight = 0;
        glfwGetWindowSize(m_window, &windowWidth, &windowHeight);

        EditorPanelVisibility panelVisibility;
        panelVisibility.hierarchy = &m_sceneHierarchyPanel->ShowPanel();
        panelVisibility.inspector = &m_sceneInspectorPanel->ShowPanel();
        panelVisibility.toolbar = &m_sceneToolbarPanel->ShowPanel();
        panelVisibility.lighting = &m_lightingPanel->ShowPanel();
        panelVisibility.performance = &m_performancePanel->ShowPanel();
        panelVisibility.project = &m_projectFilesPanel->ShowPanel();
        panelVisibility.sceneView = &m_sceneViewportPanel->ShowPanel();
        panelVisibility.gameView = &m_gameViewportPanel->ShowPanel();

        const bool allowEditorUndo =
            !IsEditorUndoRedoBlocked() && !m_playModeController.IsActive();
        const bool playActive = m_playModeController.IsActive();
        if (playActive != m_wasPlayModeActive)
        {
            m_playModeDiscardUndoStack.Clear();
            m_wasPlayModeActive = playActive;
        }

        m_mainMenuBar->Draw(
            *m_scene,
            *m_projectSession,
            *m_editorSettings,
            m_window,
            panelVisibility,
            m_projectEditorState,
            [this](ProjectEditorState& editorState) { CaptureProjectEditorState(editorState); },
            [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
            [this]() { RequestClose(); },
            [this]() { RequestNewProject(); },
            [this]() { ResetEditorLayout(); },
            [this]() {
                Scene* editorScene = GetEditorTargetScene();
                UndoStack* editorUndoStack = GetEditorUndoStack();
                if (!AlignPrimarySelectionToCameraView(*editorScene, *m_camera, editorUndoStack))
                {
                    m_projectSession->SetStatusMessage("Align to View requires a selected object.");
                }
            },
            m_playModeController,
            m_undoStack,
            m_editorClipboard,
            allowEditorUndo);

        m_editorTopToolbar->Draw(m_playModeController, *m_scene, *m_projectSession);

        Scene* editorScene = GetEditorTargetScene();
        UndoStack* editorUndoStack = GetEditorUndoStack();

        if (!m_globalEditorLayoutLoaded)
        {
            EnsureEditorLayoutLoaded();
        }

        const bool deferLayoutBuild = m_editorLayoutRestoredFromDisk && m_pendingEditorLayoutValidation;
        m_editorDockSpace->Begin(m_editorTopToolbar->GetHeight(), deferLayoutBuild);
        m_editorDockSpace->CommitLayout();
        m_sceneViewportPanel->Draw(*m_camera, *editorScene);

        Scene* gameScene = m_scene.get();
        if (m_playModeController.IsActive())
        {
            Scene* runtimeScene = m_playModeController.GetRuntimeScene();
            if (runtimeScene != nullptr)
            {
                gameScene = runtimeScene;
            }
        }

        const bool hasGameSceneCamera =
            gameScene != nullptr && SceneCamera::SceneHasActiveCamera(*gameScene);
        m_gameViewportPanel->Draw(hasGameSceneCamera, m_gameViewRenderedLastFrame);
        m_sceneHierarchyPanel->Draw(*editorScene, *m_projectSession, *editorUndoStack, m_editorClipboard);
        m_sceneInspectorPanel->Draw(*editorScene, editorUndoStack);
        m_projectFilesPanel->Draw(*m_projectSession);
        m_lightingPanel->Draw(
            *editorScene,
            *m_camera,
            m_sceneViewportPanel->GetRenderWidth(),
            m_sceneViewportPanel->GetRenderHeight(),
            editorUndoStack);
        m_performancePanel->Draw(
            *editorScene,
            m_sceneViewportPanel->GetRenderWidth(),
            m_sceneViewportPanel->GetRenderHeight(),
            windowWidth,
            windowHeight,
            m_playModeController.IsActive());
        const bool validateRestoredLayout = m_pendingEditorLayoutValidation;
        m_editorDockSpace->AfterEditorPanels(validateRestoredLayout);
        if (validateRestoredLayout)
        {
            m_pendingEditorLayoutValidation = false;
        }
        m_editorDockSpace->End();

        if (m_playModeController.ConsumeFocusGameViewRequest())
        {
            ImGui::SetWindowFocus("Game View");
        }

        const EditorViewportRect& sceneViewRect = m_sceneViewportPanel->GetInteractionRect();

        m_sceneToolbarPanel->Draw(
            *editorScene,
            m_sceneViewportPanel->ShowPanel(),
            sceneViewRect,
            editorUndoStack);

        if (m_sceneViewportPanel->HasValidRenderTarget())
        {
            m_camera->SetAspectFromFramebuffer(
                m_sceneViewportPanel->GetRenderWidth(),
                m_sceneViewportPanel->GetRenderHeight());
        }
    }

    DrawUnsavedChangesDialog();

    if (!m_input->IsCapturingMouse())
    {
        WrapImGuiMouseCursorAtWindowEdges(m_window);
    }

    const ImGuiIO& io = ImGui::GetIO();

    const bool gameViewBlocksSceneInput =
        m_playModeController.IsActive() && m_gameViewportPanel->IsHovered();
    const bool sceneInteractionHovered =
        m_sceneViewportPanel->IsHovered()
        || ImGuizmo::IsOver()
        || ImGuizmo::IsUsing()
        || ImGuizmo::IsViewManipulateHovered()
        || ImGuizmo::IsUsingViewManipulate();

    const bool sceneViewHovered =
        editorActive
        && m_sceneViewportPanel->HasValidRenderTarget()
        && sceneInteractionHovered
        && !gameViewBlocksSceneInput;
    const bool blockSceneInput = io.WantTextInput || m_pendingClose || m_pendingNewProject;

    const bool flyCameraActive = m_input->IsCapturingMouse();
    if (flyCameraActive && blockSceneInput)
    {
        m_input->ReleaseMouseCapture();
    }
    else if (io.WantCaptureMouse && !flyCameraActive && !sceneViewHovered)
    {
        m_input->ReleaseMouseCapture();
    }

    const bool allowGameKeyboard = !io.WantCaptureKeyboard || flyCameraActive;
    const bool allowSceneMouse =
        editorActive && !flyCameraActive && sceneViewHovered && !blockSceneInput;

    if (allowGameKeyboard && escapePressed)
    {
        if (m_pendingClose || m_pendingNewProject)
        {
            m_pendingClose = false;
            m_pendingNewProject = false;
            ImGui::CloseCurrentPopup();
        }
        else if (flyCameraActive)
        {
            m_input->ReleaseMouseCapture();
        }
        else if (editorActive && !cancelReorderDragOnly)
        {
            m_sceneEditingController->HandleEscapeKey(*GetEditorTargetScene());
        }
    }

    if (editorActive && flyCameraActive)
    {
        m_camera->ProcessKeyboard(*m_input, static_cast<float>(deltaTime));
        m_camera->ProcessMouseMovement(
            m_input->ConsumeMouseDeltaX(),
            m_input->ConsumeMouseDeltaY());
    }
    else
    {
        m_input->ConsumeMouseDeltaX();
        m_input->ConsumeMouseDeltaY();
    }

    if (editorActive)
    {
        m_playModeController.Simulate(deltaTime);

        int viewportWidth = 0;
        int viewportHeight = 0;
        int windowWidth = 0;
        int windowHeight = 0;
        glfwGetFramebufferSize(m_window, &viewportWidth, &viewportHeight);
        glfwGetWindowSize(m_window, &windowWidth, &windowHeight);

        const EditorViewportRect& viewportRect = m_sceneViewportPanel->GetInteractionRect();
        const EditorViewportRect* viewportPtr =
            viewportRect.valid ? &viewportRect : nullptr;

        const SceneEditorUpdateContext editorUpdateContext{
            *m_input,
            *m_camera,
            viewportWidth,
            viewportHeight,
            windowWidth,
            windowHeight,
            allowSceneMouse,
            allowGameKeyboard,
            GetEditorUndoStack(),
            m_projectSession->GetProjectRootDirectory(),
            viewportPtr};

        m_sceneEditingController->Update(*GetEditorTargetScene(), editorUpdateContext);
    }

    m_input->EndFrame();
}

void Application::RequestClose()
{
    if (!m_projectSession->HasActiveProject() || !m_projectSession->IsDirty())
    {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        return;
    }

    m_pendingClose = true;
}

void Application::RequestNewProject()
{
    if (m_playModeController.IsActive())
    {
        m_playModeController.TogglePlayStop(*m_scene, m_projectSession->GetProjectRootDirectory());
    }

    if (m_projectSession->IsDirty())
    {
        m_pendingNewProject = true;
        return;
    }

    m_projectChooser->OpenNewProjectForm(*m_editorSettings);
}

bool Application::IsEditorUndoRedoBlocked() const
{
    return m_pendingClose || m_pendingNewProject;
}

Scene* Application::GetEditorTargetScene()
{
    if (m_playModeController.IsActive())
    {
        Scene* runtimeScene = m_playModeController.GetRuntimeScene();
        if (runtimeScene != nullptr)
        {
            return runtimeScene;
        }
    }

    return m_scene.get();
}

const Scene* Application::GetEditorTargetScene() const
{
    return const_cast<Application*>(this)->GetEditorTargetScene();
}

UndoStack* Application::GetEditorUndoStack()
{
    if (m_playModeController.IsActive())
    {
        return &m_playModeDiscardUndoStack;
    }

    return &m_undoStack;
}

const UndoStack* Application::GetEditorUndoStack() const
{
    return const_cast<Application*>(this)->GetEditorUndoStack();
}

void Application::CaptureProjectEditorState(ProjectEditorState& editorState) const
{
    editorState.cameraPosition = m_camera->GetPosition();
    editorState.cameraYaw = m_camera->GetYaw();
    editorState.cameraPitch = m_camera->GetPitch();
    editorState.showHierarchy = m_sceneHierarchyPanel->ShowPanel();
    editorState.showInspector = m_sceneInspectorPanel->ShowPanel();
    editorState.showToolbar = m_sceneToolbarPanel->ShowPanel();
    editorState.showLighting = m_lightingPanel->ShowPanel();
    editorState.showPerformance = m_performancePanel->ShowPanel();
    editorState.showProjectFiles = m_projectFilesPanel->ShowPanel();
    editorState.showSceneView = m_sceneViewportPanel->ShowPanel();
    editorState.showGameView = m_gameViewportPanel->ShowPanel();
    editorState.hierarchyNodeOpenStates = m_sceneHierarchyPanel->GetNodeOpenStates();
    m_projectFilesPanel->GetBrowseState(
        editorState.projectFilesBrowsedDirectory,
        editorState.projectFilesSelectedPath,
        editorState.projectFilesFolderOpenStates);
}

void Application::ApplyProjectEditorState(const ProjectEditorState& editorState)
{
    m_camera->SetPosition(editorState.cameraPosition);
    m_camera->SetOrientation(editorState.cameraYaw, editorState.cameraPitch);

    m_sceneHierarchyPanel->ShowPanel() = editorState.showHierarchy;
    m_sceneInspectorPanel->ShowPanel() = editorState.showInspector;
    m_sceneToolbarPanel->ShowPanel() = editorState.showToolbar;
    m_lightingPanel->ShowPanel() = editorState.showLighting;
    m_performancePanel->ShowPanel() = editorState.showPerformance;
    m_projectFilesPanel->ShowPanel() = editorState.showProjectFiles;
    m_sceneViewportPanel->ShowPanel() = editorState.showSceneView;
    m_gameViewportPanel->ShowPanel() = editorState.showGameView;

    std::unordered_map<SceneObjectId, bool> hierarchyOpenStates;
    const int objectCount = static_cast<int>(m_scene->GetObjects().size());
    for (const auto& [storedKey, isOpen] : editorState.hierarchyNodeOpenStates)
    {
        if (!isOpen)
        {
            continue;
        }

        const int indexById = m_scene->FindObjectIndex(storedKey);
        if (indexById >= 0)
        {
            hierarchyOpenStates[storedKey] = true;
            continue;
        }

        const int legacyIndex = static_cast<int>(storedKey);
        if (legacyIndex >= 0 && legacyIndex < objectCount)
        {
            hierarchyOpenStates[m_scene->GetSceneObject(static_cast<std::size_t>(legacyIndex)).GetId()] = true;
        }
    }
    m_sceneHierarchyPanel->SetNodeOpenStates(hierarchyOpenStates);

    std::string browsedDirectory = editorState.projectFilesBrowsedDirectory;
    if (browsedDirectory.empty())
    {
        browsedDirectory = m_projectSession->GetProjectRootDirectory();
    }

    m_projectFilesPanel->SetBrowseState(
        browsedDirectory,
        editorState.projectFilesSelectedPath,
        editorState.projectFilesFolderOpenStates);
}

bool Application::TrySaveProject()
{
    CaptureProjectEditorState(m_projectEditorState);

    if (!m_projectSession->IsUntitled())
    {
        if (!m_projectSession->Save(*m_scene, m_projectEditorState))
        {
            return false;
        }

        EditorSettings::SaveGlobalEditorLayout();
        return true;
    }

    std::string projectPath;
    if (!FileDialog::SaveProjectFile(projectPath, m_projectSession->GetProjectFilePath()))
    {
        return false;
    }

    if (!m_projectSession->SaveAs(*m_scene, projectPath, m_projectEditorState))
    {
        return false;
    }

    m_editorSettings->AddRecentProject(m_projectSession->GetProjectFilePath());
    m_editorSettings->SetLastNewProjectParentDirectoryFromProjectFile(m_projectSession->GetProjectFilePath());
    m_editorSettings->Save();
    EditorSettings::SaveGlobalEditorLayout();
    return true;
}

void Application::ResetEditorLayout()
{
    EditorSettings::DeleteGlobalImGuiIni();
    m_editorDockSpace->ResetLayout();
    m_editorLayoutRestoredFromDisk = false;
    m_pendingEditorLayoutValidation = false;
}

void Application::EnsureEditorLayoutLoaded()
{
    if (m_globalEditorLayoutLoaded)
    {
        return;
    }

    try
    {
        ImGui::ClearIniSettings();
        m_editorLayoutRestoredFromDisk = EditorSettings::LoadGlobalEditorLayout();
        if (!m_editorLayoutRestoredFromDisk
            && m_projectSession->HasActiveProject()
            && EditorSettings::TryMigrateProjectEditorLayout(m_projectSession->GetProjectRootDirectory()))
        {
            m_editorLayoutRestoredFromDisk = true;
        }

        if (!m_editorLayoutRestoredFromDisk)
        {
            EngineLog::Warn("editor", "No saved editor layout found; using default layout.");
            m_editorDockSpace->RequestLayoutRebuild();
        }
        else
        {
            m_pendingEditorLayoutValidation = true;
        }
    }
    catch (const std::exception& exception)
    {
        EngineLog::LogException("editor", "LoadGlobalEditorLayout", exception);
        EditorSettings::DeleteGlobalImGuiIni();
        m_editorDockSpace->ResetLayout();
        m_editorLayoutRestoredFromDisk = false;
        m_pendingEditorLayoutValidation = false;
    }

    m_globalEditorLayoutLoaded = true;
}

void Application::DrawUnsavedChangesDialog()
{
    if (!m_pendingClose && !m_pendingNewProject)
    {
        return;
    }

    const bool isClosePrompt = m_pendingClose;
    ImGui::OpenPopup("Unsaved Changes");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(
            "Unsaved Changes",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    }

    if (isClosePrompt)
    {
        ImGui::TextUnformatted("Save changes before closing?");
    }
    else
    {
        ImGui::TextUnformatted("Save changes before creating a new project?");
    }

    ImGui::Separator();

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
    {
        if (TrySaveProject())
        {
            if (isClosePrompt)
            {
                m_pendingClose = false;
                ImGui::CloseCurrentPopup();
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            else
            {
                m_pendingNewProject = false;
                ImGui::CloseCurrentPopup();
                m_undoStack.Clear();
                m_editorClipboard.Clear();
                m_projectSession->CloseProject();
                m_projectChooser->OpenNewProjectForm(*m_editorSettings);
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(120.0f, 0.0f)))
    {
        if (isClosePrompt)
        {
            m_pendingClose = false;
            ImGui::CloseCurrentPopup();
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        }
        else
        {
            m_pendingNewProject = false;
            ImGui::CloseCurrentPopup();
            m_undoStack.Clear();
            m_editorClipboard.Clear();
            m_projectSession->CloseProject();
            m_projectChooser->OpenNewProjectForm(*m_editorSettings);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
    {
        m_pendingClose = false;
        m_pendingNewProject = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
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

void Application::Render()
{
    if (GfxContext::Get().IsDeviceRemoved())
    {
        if (m_imguiFrameActive)
        {
            try
            {
                m_imguiLayer->CancelInterruptedFrame();
            }
            catch (...)
            {
            }

            m_imguiFrameActive = false;
        }

        return;
    }

    m_gfxFrameActive = true;
    RunApplicationPhase("render-begin", [&]() {
        FrameDiagnostics::LogPhase("render-begin");
        m_renderer->BeginFrame();
    });

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();
    if (editorActive)
    {
        GfxContext::Get().WaitForSwapchainFrames();
    }

    m_gameViewRenderedLastFrame = false;

    if (editorActive && m_sceneViewportPanel->HasValidRenderTarget())
    {
        RunApplicationPhase("scene-view-render", [&]() {
            FrameDiagnostics::LogPhase("scene-view-render");
            Scene* sceneViewScene = GetEditorTargetScene();
            m_sceneViewportPanel->EnsureFramebufferSized();
            if (m_sceneViewportPanel->HasGpuFramebuffer())
            {
                m_camera->SetAspectFromFramebuffer(
                    m_sceneViewportPanel->GetRenderWidth(),
                    m_sceneViewportPanel->GetRenderHeight());
                sceneViewScene->Render(
                    *m_camera,
                    m_sceneViewportPanel->GetRenderWidth(),
                    m_sceneViewportPanel->GetRenderHeight(),
                    m_sceneViewportPanel->GetFramebuffer());
            }
        });
    }

    if (editorActive && m_gameViewportPanel->HasValidRenderTarget())
    {
        RunApplicationPhase("game-view-render", [&]() {
            Scene* gameScene = m_scene.get();
            if (m_playModeController.IsActive())
            {
                Scene* runtimeScene = m_playModeController.GetRuntimeScene();
                if (runtimeScene != nullptr)
                {
                    gameScene = runtimeScene;
                }
            }

            const int gameViewWidth = m_gameViewportPanel->GetRenderWidth();
            const int gameViewHeight = m_gameViewportPanel->GetRenderHeight();
            const float gameViewAspect =
                gameViewHeight > 0
                    ? static_cast<float>(gameViewWidth) / static_cast<float>(gameViewHeight)
                    : 1.0f;

            if (gameScene != nullptr)
            {
                const std::optional<SceneCamera> sceneCamera =
                    SceneCamera::TryFromScene(*gameScene, gameViewAspect);
                if (sceneCamera.has_value())
                {
                    m_gameViewportPanel->EnsureFramebufferSized();
                    if (m_gameViewportPanel->HasGpuFramebuffer())
                    {
                        const Camera renderCamera = sceneCamera->ToRenderCamera();
                        const SceneRenderOptions gameViewOptions{
                            false,
                            false,
                            false,
                            false,
                            false,
                        };
                        gameScene->Render(
                            renderCamera,
                            gameViewWidth,
                            gameViewHeight,
                            m_gameViewportPanel->GetFramebuffer(),
                            gameViewOptions);
                        m_gameViewRenderedLastFrame = true;
                    }
                }
            }
        });
    }

    RunApplicationPhase("imgui-end", [&]() {
        FrameDiagnostics::LogPhase("imgui-end");
        m_imguiLayer->EndFrame();
    });
    RunApplicationPhase("present", [&]() {
        FrameDiagnostics::LogPhase("present");
        m_renderer->EndFrame(m_window);
    });
    if (editorActive)
    {
        m_projectChooser->CompleteDeferredProjectLoadProgress();
    }
    m_imguiFrameActive = false;
    m_gfxFrameActive = false;
}
