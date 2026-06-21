#pragma once

#include <memory>

struct GLFWwindow;
class Camera;
class DebugPanel;
class DemoScene;
class ImGuiLayer;
class Input;
class Renderer;
class SceneHierarchyPanel;

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
    bool m_paused = false;

    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<ImGuiLayer> m_imguiLayer;
    std::unique_ptr<DebugPanel> m_debugPanel;
    std::unique_ptr<SceneHierarchyPanel> m_sceneHierarchyPanel;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Input> m_input;
    std::unique_ptr<DemoScene> m_scene;
};
