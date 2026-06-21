#pragma once

struct GLFWwindow;

class ImGuiLayer
{
public:
    explicit ImGuiLayer(GLFWwindow* window);
    ~ImGuiLayer();

    void BeginFrame();
    void EndFrame();

private:
    GLFWwindow* m_window = nullptr;
};
