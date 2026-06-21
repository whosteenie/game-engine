#pragma once

#include <glm/glm.hpp>
#include <memory>

struct GLFWwindow;
class Camera;
class Mesh;
class Renderer;
class Shader;

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
    void HandleInput(double deltaTime);
    void OnFramebufferResize(int width, int height);

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);

    int m_width;
    int m_height;
    const char* m_title;

    GLFWwindow* m_window = nullptr;
    double m_animationTime = 0.0;
    bool m_paused = false;
    bool m_spaceWasDown = false;

    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Shader> m_shader;
    std::unique_ptr<Mesh> m_mesh;

    glm::vec3 m_position = glm::vec3(0.0f);
};
