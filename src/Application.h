#pragma once

struct GLFWwindow;
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
    void CreateTriangleMesh();
    void DestroyTriangleMesh();

    void Update(double deltaTime);
    void Render();
    void HandleInput();
    void OnFramebufferResize(int width, int height);

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);

    int m_width;
    int m_height;
    const char* m_title;

    GLFWwindow* m_window = nullptr;
    float m_aspect = 1.0f;
    double m_animationTime = 0.0;
    bool m_paused = false;
    bool m_spaceWasDown = false;

    Shader* m_shader = nullptr;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
};
