#pragma once

struct GLFWwindow;

#include <string>

class ImGuiLayer
{
public:
    explicit ImGuiLayer(GLFWwindow* window, const std::string& iniPath = {});
    ~ImGuiLayer();

    // Install the GLFW platform backend after the native window and GPU swapchain exist.
    void InitPlatformBackend();
    void BeginFrame();
    void EndFrame();

private:
    GLFWwindow* m_window = nullptr;
    std::string m_iniPath;
};
