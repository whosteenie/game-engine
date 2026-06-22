#pragma once

#include <memory>

struct GLFWwindow;
class Camera;
class LightingPanel;
class MainMenuBar;
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

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);

    static void MouseCallback(GLFWwindow* window, double xPos, double yPos);

    int m_width;
    int m_height;
    const char* m_title;

    GLFWwindow* m_window = nullptr;

    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<ImGuiLayer> m_imguiLayer;
    std::unique_ptr<ProjectSession> m_projectSession;
    std::unique_ptr<MainMenuBar> m_mainMenuBar;
    std::unique_ptr<LightingPanel> m_lightingPanel;
    std::unique_ptr<SceneToolbarPanel> m_sceneToolbarPanel;
    std::unique_ptr<SceneHierarchyPanel> m_sceneHierarchyPanel;
    std::unique_ptr<SceneInspectorPanel> m_sceneInspectorPanel;
    std::unique_ptr<ProjectFilesPanel> m_projectFilesPanel;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<Scene> m_scene;
};
