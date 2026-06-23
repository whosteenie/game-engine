#pragma once

#include "app/ProjectEditorState.h"
#include "app/UndoStack.h"

#include <memory>

struct GLFWwindow;
class Camera;
class EditorSettings;
class LightingPanel;
class MainMenuBar;
class ProjectChooser;
class ProjectFilesPanel;
class ProjectSession;
class Scene;
class ImGuiLayer;
class Input;
class Renderer;
class SceneHierarchyPanel;
class SceneInspectorPanel;
class SceneToolbarPanel;

class Application
{
public:
    Application(int width, int height, const char* title);
    ~Application();

    void Run();

private:
    void InitGLFW();
    void InitGLAD();

    void Update(double deltaTime);
    void Render();
    void OnFramebufferResize(int width, int height);
    void RequestClose();
    void RequestNewProject();
    void DrawUnsavedChangesDialog();
    void CaptureProjectEditorState(ProjectEditorState& editorState) const;
    void ApplyProjectEditorState(const ProjectEditorState& editorState);
    bool TrySaveProject();

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void WindowCloseCallback(GLFWwindow* window);
    static void MouseCallback(GLFWwindow* window, double xPos, double yPos);

    bool m_pendingClose = false;
    bool m_pendingNewProject = false;

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
    std::unique_ptr<LightingPanel> m_lightingPanel;
    std::unique_ptr<SceneToolbarPanel> m_sceneToolbarPanel;
    std::unique_ptr<SceneHierarchyPanel> m_sceneHierarchyPanel;
    std::unique_ptr<SceneInspectorPanel> m_sceneInspectorPanel;
    std::unique_ptr<ProjectFilesPanel> m_projectFilesPanel;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<Scene> m_scene;
    UndoStack m_undoStack;
    ProjectEditorState m_projectEditorState;
};
