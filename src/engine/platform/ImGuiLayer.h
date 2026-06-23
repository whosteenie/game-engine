#pragma once

struct GLFWwindow;

#include <string>

class ImGuiLayer
{
public:
    explicit ImGuiLayer(GLFWwindow* window, const std::string& iniPath = {});
    ~ImGuiLayer();

    void BeginFrame();
    void EndFrame();

private:
    GLFWwindow* m_window = nullptr;
    std::string m_iniPath;
};
