#pragma once

#include "app/project/ProjectEditorState.h"
#include "app/core/PlayModeController.h"
#include "app/undo/UndoStack.h"
#include "app/panels/PerformancePanel.h"

#include "app/editor/EditorClipboard.h"

#include <memory>

struct GLFWwindow;
class Camera;
class EditorDockSpace;
class EditorTopToolbar;
class EditorSettings;
class LightingPanel;
class MainMenuBar;
class ProjectChooser;
class ProjectFilesPanel;
class ProjectSession;
class Scene;
class SceneEditingController;
class ImGuiLayer;
class Input;
class Renderer;
class SceneHierarchyPanel;
class SceneInspectorPanel;
class SceneToolbarPanel;
class SceneViewportPanel;
class GameViewportPanel;

class Application
{
public:
    Application(int width, int height, const char* title);
    ~Application();

    void Run();

private:
    void InitGLFW();

    void Update(double deltaTime, ApplicationFrameDiagnostics& frameDiagnostics);
    void Render();
    void OnFramebufferResize(int width, int height);
    void RequestClose();
    void RequestForcedClose();
    void RequestNewProject();
    void RequestOpenProject();
    void DrawUnsavedChangesDialog();
    void CaptureProjectEditorState(ProjectEditorState& editorState) const;
    void ApplyProjectEditorState(const ProjectEditorState& editorState);
    bool TrySaveProject();
    bool IsEditorUndoRedoBlocked() const;
    void ResetEditorLayout();
    void ResetEditorLayoutLoadState();
    void RecoverInterruptedFrame();
    void EnsureEditorLayoutLoaded();
    void HandleFatalGpuDeviceLoss(const std::string& reason);
    void PumpStartupFramesUntilDlssReady();
    void UpdatePendingProjectStartupProgress(const char* message) const;
    void ProcessQueuedProjectOpenIfReady();
    void ProcessPendingProjectTeardown();
    void ApplyS1p6CaptureModeIfRequested();
    void ApplyS2p1CaptureModeIfRequested();
    void ApplyS2p4CaptureModeIfRequested();
    bool RunS2p2ExtentQueryMatrixIfRequested();

    Scene* GetEditorTargetScene();
    const Scene* GetEditorTargetScene() const;
    UndoStack* GetEditorUndoStack();
    const UndoStack* GetEditorUndoStack() const;

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void WindowCloseCallback(GLFWwindow* window);

    bool m_pendingClose = false;
    bool m_pendingNewProject = false;
    bool m_pendingOpenProject = false;
    bool m_pendingProjectTeardown = false;
    bool m_fatalGpuLossHandled = false;
    bool m_projectLoadBenchmarkAwaitingGpuCompletion = false;

    int m_width;
    int m_height;
    const char* m_title;

    GLFWwindow* m_window = nullptr;

    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<ImGuiLayer> m_imguiLayer;
    std::unique_ptr<EditorSettings> m_editorSettings;
    std::unique_ptr<ProjectSession> m_projectSession;
    std::unique_ptr<ProjectChooser> m_projectChooser;
    std::unique_ptr<MainMenuBar> m_mainMenuBar;
    std::unique_ptr<EditorTopToolbar> m_editorTopToolbar;
    std::unique_ptr<LightingPanel> m_lightingPanel;
    std::unique_ptr<PerformancePanel> m_performancePanel;
    std::unique_ptr<SceneToolbarPanel> m_sceneToolbarPanel;
    std::unique_ptr<SceneHierarchyPanel> m_sceneHierarchyPanel;
    std::unique_ptr<SceneInspectorPanel> m_sceneInspectorPanel;
    std::unique_ptr<ProjectFilesPanel> m_projectFilesPanel;
    std::unique_ptr<SceneViewportPanel> m_sceneViewportPanel;
    std::unique_ptr<GameViewportPanel> m_gameViewportPanel;
    std::unique_ptr<EditorDockSpace> m_editorDockSpace;
    std::unique_ptr<class AutomatedBenchmarkCapture> m_automatedBenchmarkCapture;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<SceneEditingController> m_sceneEditingController;
    PlayModeController m_playModeController;
    UndoStack m_playModeDiscardUndoStack;
    bool m_wasPlayModeActive = false;
    bool m_imguiFrameActive = false;
    bool m_gfxFrameActive = false;
    bool m_globalEditorLayoutLoaded = false;
    bool m_editorLayoutRestoredFromDisk = false;
    bool m_pendingEditorLayoutValidation = false;
    int m_editorLayoutStabilizationFrames = 0;
    bool m_automationDualViewportLayout = false;
    bool m_s1p6CaptureModeApplied = false;
    bool m_s2p1CaptureModeApplied = false;
    bool m_s2p4CaptureModeApplied = false;
    bool m_s2p2ExtentQueryMatrixComplete = false;
    UndoStack m_undoStack;
    EditorClipboard m_editorClipboard;
    ProjectEditorState m_projectEditorState;
};
