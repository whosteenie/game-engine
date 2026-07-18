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
#include "app/core/benchmark/Capture.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/EditorDockSpace.h"
#include "app/editor/EditorPanelConstraints.h"
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
#include "app/project/ProjectViewportRevealGate.h"
#include "app/panels/ProjectFilesPanel.h"
#include "app/project/ProjectSession.h"
#include "app/project/SceneProjectIODetail.h"
#include "app/scene/document/Scene.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "app/scene/editing/SceneEditingController.h"
#include "app/scene/editing/SceneEditor.h"
#include "app/scene/editing/SceneEditorUpdateContext.h"
#include "app/panels/SceneHierarchyPanel.h"
#include "app/panels/SceneInspectorPanel.h"
#include "app/scene/editing/SceneCamera.h"
#include "app/panels/SceneToolbarPanel.h"
#include "app/panels/SceneViewportPanel.h"
#include "app/undo/UndoContext.h"
#include "app/undo/UndoStack.h"
#include "engine/camera/Camera.h"
#include "engine/scene/RotationUtils.h"
#include "engine/scene/SceneObject.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/assets/FileDialog.h"
#include "engine/assets/TextureCache.h"
#include "app/editor/TuningSectionState.h"
#include "engine/platform/ui/ImGuiLayer.h"
#include "engine/platform/diagnostics/EngineLog.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/HresultFormat.h"

#include <ImGuizmo.h>
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/platform/tooling/ProjectLoadProgress.h"
#include "engine/platform/input/Input.h"
#include "engine/platform/input/InputDiagnostics.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/rendering/core/Renderer.h"

#include <imgui_impl_glfw.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <typeinfo>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>
#include <unordered_map>

#include "app/core/application/Detail.h"

namespace
{

    bool AlignPrimarySelectionToCameraView(Scene& scene, const Camera& camera, UndoStack* undoStack)
    {
        const int selectedIndex = scene.GetPrimarySelection();
        if (selectedIndex < 0
            || static_cast<std::size_t>(selectedIndex) >= scene.GetObjects().size())
        {
            return false;
        }

        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(selectedIndex));
        const glm::mat4 inverseViewMatrix = glm::inverse(camera.GetViewMatrix());
        const glm::mat4 cameraWorldMatrix = object.HasCamera()
            ? RotationUtils::BuildCameraObjectWorldMatrixFromEditorViewInverse(inverseViewMatrix)
            : inverseViewMatrix;
        glm::mat4 localMatrix = cameraWorldMatrix;
        if (object.GetParentIndex() >= 0)
        {
            const glm::mat4 parentWorldMatrix = scene.GetWorldMatrix(object.GetParentIndex());
            localMatrix = glm::inverse(parentWorldMatrix) * cameraWorldMatrix;
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

void Application::Update(double deltaTime, ApplicationFrameDiagnostics& frameDiagnostics)
{
    m_performancePanel->OnFrame(deltaTime);

    glfwPollEvents();
    InputDiagnostics::LogFrame(m_window, "after-poll");

    if (GfxContext::Get().IsInitialized())
    {
        std::string deviceRemovedReason;
        if (GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason))
        {
            HandleFatalGpuDeviceLoss(HresultFormat::FatalDeviceRemovedMessage(deviceRemovedReason));
            return;
        }

        GfxContext::Get().TryDeferredStreamlineSwapChainUpgrade();
    }

    ProcessPendingProjectTeardown();

    const bool escapePressed = m_input->WasKeyPressed(GLFW_KEY_ESCAPE);
    const bool cancelReorderDragOnly =
        escapePressed && EditorReorderDragDrop::IsReorderDragActive();

    // Apply deferred GPU-structural changes (e.g. geometry MSAA reload) here, before ImGui begins a
    // new frame. Recreating pipelines/framebuffers while the UI is mid-build leaves ImGui draw data
    // referencing destroyed resources, which faults the GPU. This is the safe frame boundary.
    if (Scene* pendingScene = GetEditorTargetScene())
    {
        SceneRenderer& pendingRenderer = pendingScene->GetRenderer();
        if (pendingRenderer.IsGeometryMsaaReloadRequested() && pendingRenderer.IsGpuResourcesReady())
        {
            ApplicationDetail::RunPhase("apply-geometry-msaa-reload", [&]() {
                pendingRenderer.ApplyGeometryMsaaReload(
                    *pendingScene,
                    m_sceneViewportPanel->GetRenderWidth(),
                    m_sceneViewportPanel->GetRenderHeight());
            });
        }
    }

    const auto imguiBeginStart = std::chrono::steady_clock::now();
    m_imguiLayer->BeginFrame();
    frameDiagnostics.imguiBeginCpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - imguiBeginStart).count();
    m_imguiFrameActive = true;
    InputDiagnostics::LogFrame(m_window, "after-imgui-newframe");

    const auto projectChooserUiStart = std::chrono::steady_clock::now();
    m_projectChooser->Draw(
        *m_projectSession,
        *m_scene,
        *m_editorSettings,
        m_projectEditorState,
        [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
        [this]() { RequestClose(); },
        [this]() { ResetEditorLayoutLoadState(); },
        m_undoStack,
        m_editorClipboard);
    frameDiagnostics.projectChooserUiCpuMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - projectChooserUiStart).count();

    ProcessQueuedProjectOpenIfReady();

    InputDiagnostics::LogFrame(m_window, "after-ui-build");

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();

    const bool blockSceneInputEarly = m_pendingClose || m_pendingNewProject || m_pendingOpenProject
        || m_projectChooser->IsPresentingProjectLoad();
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

        // Commit any active inspector text field before scene-view interaction (fly cam / pick).
        // Otherwise WantTextInput stays true and immediately cancels mouse capture.
        //
        // Gate on the PRESS EDGE (button transitioning down this frame), not the held-down level.
        // A held-down level check fires every frame the button stays down, so dragging a slider
        // from a panel across the viewport (still holding LMB) would hit IsAnyItemActive() and
        // ClearActiveID() the slider's grab mid-drag, invalidating it until re-grabbed. Only a
        // press that STARTS while hovering the viewport should steal/commit the active widget.
        ImGuiIO& earlyIo = ImGui::GetIO();
        const float flySpeedScroll = earlyIo.MouseWheel;
        const bool sceneViewMousePressed =
            mouseOverSceneView
            && (m_input->WasMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT)
                || m_input->WasMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT));
        if (sceneViewMousePressed && (earlyIo.WantTextInput || ImGui::IsAnyItemActive()))
        {
            ImGui::ClearActiveID();
        }

        m_input->UpdateMouseCapture(allowFlyCameraCapture);
        if (m_input->IsCapturingMouse())
        {
            if (flySpeedScroll != 0.0f)
            {
                m_camera->AdjustFlySpeed(flySpeedScroll);
                m_sceneViewportPanel->NotifyFlySpeedChanged(m_camera->GetFlySpeed());
            }
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
            if (!playActive && m_scene != nullptr)
            {
                m_scene->GetRenderer().InvalidateGameViewMotionOnPlayStop();
            }
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
            [this](const std::string& projectPath) {
                return m_projectChooser->QueueProjectOpen(projectPath);
            },
            [this]() { RequestClose(); },
            [this]() { RequestNewProject(); },
            [this]() { RequestOpenProject(); },
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

        const bool gameViewWillRender =
            EditorPanelConstraints::IsViewportTabSelected("Game View")
            && m_gameViewportPanel->ShowPanel()
            && hasGameSceneCamera
            && gameScene != nullptr
            && gameScene->GetRenderer().IsGpuResourcesReady();

        const bool sceneViewWillRender =
            EditorPanelConstraints::IsViewportTabSelected("Scene View")
            && m_sceneViewportPanel->ShowPanel()
            && editorScene != nullptr
            && editorScene->GetRenderer().IsGpuResourcesReady();

        EditorPanelConstraints::SyncViewportDockVisibleWindow("Scene View", "Game View");

        const auto viewportUiStart = std::chrono::steady_clock::now();
        m_gameViewportPanel->Draw(hasGameSceneCamera, gameViewWillRender);
        m_sceneViewportPanel->Draw(
            *m_camera,
            *editorScene,
            *m_projectSession,
            *editorUndoStack,
            sceneViewWillRender);
        frameDiagnostics.viewportUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - viewportUiStart).count();

        const auto hierarchyUiStart = std::chrono::steady_clock::now();
        m_sceneHierarchyPanel->Draw(*editorScene, *m_projectSession, *editorUndoStack, m_editorClipboard);
        frameDiagnostics.hierarchyUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - hierarchyUiStart).count();

        const auto inspectorUiStart = std::chrono::steady_clock::now();
        m_sceneInspectorPanel->Draw(*editorScene, editorUndoStack);
        frameDiagnostics.inspectorUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - inspectorUiStart).count();

        const auto projectFilesUiStart = std::chrono::steady_clock::now();
        m_projectFilesPanel->Draw(*editorScene, *m_projectSession, *editorUndoStack);
        frameDiagnostics.projectFilesUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - projectFilesUiStart).count();

        const auto lightingUiStart = std::chrono::steady_clock::now();
        m_lightingPanel->Draw(
            *editorScene,
            *m_camera,
            m_sceneViewportPanel->GetRenderWidth(),
            m_sceneViewportPanel->GetRenderHeight(),
            editorUndoStack,
            m_editorSettings.get());
        frameDiagnostics.lightingUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - lightingUiStart).count();

        const auto performanceUiStart = std::chrono::steady_clock::now();
        m_performancePanel->Draw(
            *editorScene,
            editorScene->GetRenderer(),
            m_sceneViewportPanel->GetRenderWidth(),
            m_sceneViewportPanel->GetRenderHeight(),
            windowWidth,
            windowHeight,
            m_playModeController.IsActive());
        frameDiagnostics.performanceUiCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - performanceUiStart).count();
        const bool validateRestoredLayout = m_pendingEditorLayoutValidation;
        m_editorDockSpace->AfterEditorPanels(validateRestoredLayout);
        if (validateRestoredLayout)
        {
            m_pendingEditorLayoutValidation = false;
        }
        if (m_editorLayoutStabilizationFrames > 0)
        {
            --m_editorLayoutStabilizationFrames;
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

    // Project loading builds the real editor layout behind the startup screen. Restore the chooser
    // to the front after the editor windows have submitted their draw data so the background does
    // not transition until the stable first project frame has actually completed.
    m_projectChooser->RaiseProjectLoadPresentation();

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
    // WantTextInput must not cancel an in-progress fly-cam capture after we ClearActiveID above;
    // io may still report text focus until widgets rebuild later this frame.
    const bool flyCameraActive = m_input->IsCapturingMouse();
    const bool blockSceneInput =
        (!flyCameraActive && io.WantTextInput)
        || m_pendingClose || m_pendingNewProject || m_pendingOpenProject;

    if (flyCameraActive && (m_pendingClose || m_pendingNewProject || m_pendingOpenProject))
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
        if (m_pendingClose || m_pendingNewProject || m_pendingOpenProject)
        {
            m_pendingClose = false;
            m_pendingNewProject = false;
            m_pendingOpenProject = false;
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
        if (!m_playModeController.IsActive() && !m_playModeController.GetLastError().empty())
        {
            m_projectSession->SetStatusMessage(m_playModeController.GetLastError());
        }

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

        const auto sceneEditorStart = std::chrono::steady_clock::now();
        m_sceneEditingController->Update(*GetEditorTargetScene(), editorUpdateContext);
        frameDiagnostics.sceneEditorCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sceneEditorStart).count();
    }

    m_input->EndFrame();
}

void Application::Render()
{
    if (GfxContext::Get().IsDeviceRemoved())
    {
        std::string deviceRemovedReason;
        (void)GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason);
        HandleFatalGpuDeviceLoss(HresultFormat::FatalDeviceRemovedMessage(deviceRemovedReason));
        return;
    }

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();
    const bool presentingProjectLoad = m_projectChooser->IsPresentingProjectLoad();
    const bool projectLoadBenchmarkActive = ProjectLoadBenchmark::IsActive();
    const bool projectLayoutStable =
        !presentingProjectLoad
        || (m_editorLayoutStabilizationFrames == 0 && !m_pendingEditorLayoutValidation);

    if (editorActive || presentingProjectLoad)
    {
        if (Scene* editorScene = GetEditorTargetScene())
        {
            ApplicationDetail::RunPhase("apply-deferred-renderer-settings", [&]() {
                SceneProjectIODetail::ApplyDeferredRendererSettings(*editorScene);
            });
            ApplyS1p6CaptureModeIfRequested();
            ApplyS2p1CaptureModeIfRequested();
            ApplyS2p4CaptureModeIfRequested();
            ApplicationDetail::RunPhase("prepare-frame-gpu", [&]() {
                ProjectLoadBenchmark::ScopedPhase projectLoadGpuPrepare(
                    presentingProjectLoad ? "renderer.first_gpu_prepare" : nullptr);
                if (presentingProjectLoad)
                {
                    ProjectLoadProgress::Report(
                        "Preparing GPU resources for first frame...",
                        ProjectLoadProgress::kGpuInitializationStart);
                }
                editorScene->GetRenderer().PrepareFrameGpuResources();
            });
        }

        if (EditorPanelConstraints::IsViewportTabSelected("Game View")
            && m_gameViewportPanel->HasValidRenderTarget())
        {
            ApplicationDetail::RunPhase("prepare-game-view-gpu", [&]() {
                Scene* gameScene = m_scene.get();
                if (m_playModeController.IsActive())
                {
                    if (Scene* runtimeScene = m_playModeController.GetRuntimeScene())
                    {
                        gameScene = runtimeScene;
                    }
                }

                if (gameScene != nullptr)
                {
                    gameScene->GetRenderer().PrepareGameViewGpuResources();
                }
            });
        }
    }

    if (projectLoadBenchmarkActive && editorActive)
    {
        // Automated captures explicitly require a Scene View target even when a saved layout has
        // that tab hidden. Interactive project loading always uses the real settled panel size.
        m_sceneViewportPanel->EnsureBenchmarkRenderTarget(m_width, m_height);
    }

    m_gfxFrameActive = true;
    ApplicationDetail::RunPhase("render-begin", [&]() {
        FrameDiagnostics::BeginApplicationFrame();
        FrameDiagnostics::LogPhase("render-begin");
        m_renderer->BeginFrame();
    });

    const bool sceneViewWillRender = editorActive && projectLayoutStable
        && (projectLoadBenchmarkActive
            || EditorPanelConstraints::IsViewportTabSelected("Scene View"))
        && m_sceneViewportPanel->HasValidRenderTarget()
        && (projectLoadBenchmarkActive || !m_sceneViewportPanel->IsLiveResizePending());
    if (sceneViewWillRender)
    {
        ApplicationDetail::RunPhase("scene-view-render", [&]() {
            FrameDiagnostics::LogPhase("scene-view-render");
            SceneRenderTrace::Scope sceneViewScope("scene-view-render");
            Scene* sceneViewScene = GetEditorTargetScene();
            m_sceneViewportPanel->EnsureFramebufferSized();
            if (sceneViewScene != nullptr && m_sceneViewportPanel->HasGpuFramebuffer()
                && sceneViewScene->GetRenderer().IsGpuResourcesReady())
            {
                if (presentingProjectLoad)
                {
                    ProjectLoadProgress::Report(
                        "Rendering Scene View first frame...",
                        ProjectLoadProgress::kFirstSceneFrameStart);
                }
                SceneRenderTrace::FirstFrameGuard firstFrameGuard;
                m_camera->SetAspectFromFramebuffer(
                    m_sceneViewportPanel->GetRenderWidth(),
                    m_sceneViewportPanel->GetRenderHeight());
                SceneRenderOptions sceneViewOptions{};
                sceneViewOptions.shadingMode = m_sceneToolbarPanel->GetShadingMode();
                {
                    ProjectLoadBenchmark::ScopedPhase firstSceneRenderPhase(
                        projectLoadBenchmarkActive ? "renderer.first_scene_render_record" : nullptr);
                    sceneViewScene->Render(
                        *m_camera,
                        m_sceneViewportPanel->GetRenderWidth(),
                        m_sceneViewportPanel->GetRenderHeight(),
                        m_sceneViewportPanel->GetFramebuffer(),
                        sceneViewOptions,
                        RenderViewport::SceneView);
                }
                if (presentingProjectLoad)
                {
                    ProjectLoadProgress::Report(
                        "Compositing Scene View...",
                        ProjectLoadProgress::kSceneComposite);
                }
                m_sceneViewportPanel->CompositeRenderedFrame();
                if (projectLoadBenchmarkActive)
                {
                    ProjectLoadBenchmark::Mark("scene_view.first_composite_recorded");
                    m_projectLoadBenchmarkAwaitingGpuCompletion = true;
                }
            }
            sceneViewScope.Success();
        });
    }
    else
    {
        const char* const reason = m_sceneViewportPanel->IsLiveResizePending()
            ? "viewport-live-resize"
            : "scene-view-not-rendered";
        FrameDiagnostics::LogDlssEvent(
            0, "not-evaluated", "not-evaluated", "skipped", reason,
            false, 0, false, 0);
    }

    const bool gameViewWillRender = editorActive && projectLayoutStable
        && EditorPanelConstraints::IsViewportTabSelected("Game View")
        && m_gameViewportPanel->HasValidRenderTarget()
        && !m_gameViewportPanel->IsLiveResizePending();
    if (gameViewWillRender)
    {
        ApplicationDetail::RunPhase("game-view-render", [&]() {
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
                        if (presentingProjectLoad)
                        {
                            ProjectLoadProgress::Report(
                                "Rendering Game View first frame...",
                                ProjectLoadProgress::kGameViewFirstFrame);
                        }
                        const Camera renderCamera = sceneCamera->ToRenderCamera();
                        const SceneRenderOptions gameViewOptions{
                            false,
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
                            gameViewOptions,
                            RenderViewport::GameView);
                        if (presentingProjectLoad)
                        {
                            ProjectLoadProgress::Report(
                                "Compositing Game View...",
                                ProjectLoadProgress::kGameViewComposite);
                        }
                        m_gameViewportPanel->CompositeRenderedFrame();
                    }
                }
            }
        });
    }
    else
    {
        const char* const reason = m_gameViewportPanel->IsLiveResizePending()
            ? "viewport-live-resize"
            : "game-view-not-rendered";
        FrameDiagnostics::LogDlssEvent(
            1, "not-evaluated", "not-evaluated", "skipped", reason,
            false, 0, false, 0);
    }

    if (presentingProjectLoad)
    {
        const bool sceneImageRequired = m_sceneViewportPanel->ShowPanel()
            && EditorPanelConstraints::IsViewportTabSelected("Scene View");
        const bool gameImageRequired = m_gameViewportPanel->ShowPanel()
            && EditorPanelConstraints::IsViewportTabSelected("Game View")
            && m_gameViewportPanel->HasSceneCamera();
        const ProjectViewportRevealState revealState{
            projectLayoutStable,
            sceneImageRequired,
            m_sceneViewportPanel->HasReadyCompositeFrame(),
            gameImageRequired,
            m_gameViewportPanel->HasReadyCompositeFrame(),
        };
        // Non-viewport tabs and Game View without a scene camera have an intentional editor
        // placeholder. Selected image-producing viewports must submit a composite first.
        if (CanRevealProjectEditor(revealState))
        {
            m_projectChooser->NotifyEditorCompositeReady();
        }
        const Scene* editorScene = GetEditorTargetScene();
        const bool gpuResourcesFailed =
            editorScene != nullptr && editorScene->GetRenderer().HasGpuResourcesInitFailed();
        if (gpuResourcesFailed && projectLoadBenchmarkActive)
        {
            ProjectLoadBenchmark::Fail(editorScene->GetRenderer().GetGpuResourcesInitError());
            RequestForcedClose();
        }
        m_projectChooser->TickProjectLoadTimeout(gpuResourcesFailed);
    }

    ApplicationDetail::RunPhase("imgui-end", [&]() {
        FrameDiagnostics::LogPhase("imgui-end");
        m_imguiLayer->EndFrame();
    });
    ApplicationDetail::RunPhase("present", [&]() {
        FrameDiagnostics::LogPhase("present");
        m_renderer->EndFrame(m_window);
        FrameDiagnostics::EndApplicationFrame();
    });
    if (m_projectLoadBenchmarkAwaitingGpuCompletion && ProjectLoadBenchmark::IsActive())
    {
        {
            ProjectLoadBenchmark::ScopedPhase waitForFirstFramePhase("renderer.first_frame_gpu_complete");
            GfxContext::Get().WaitForSwapchainFrames(false);
        }
        ProjectLoadBenchmark::Mark("scene_view.first_frame_gpu_complete");
        ProjectLoadBenchmark::Complete();
        m_projectLoadBenchmarkAwaitingGpuCompletion = false;
        RequestForcedClose();
    }
    m_projectChooser->FinishScheduledPresentation();
    m_imguiFrameActive = false;
    m_gfxFrameActive = false;
}

