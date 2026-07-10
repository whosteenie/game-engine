#pragma once

struct GLFWwindow;

namespace InputDiagnostics
{
    bool IsEnabled();

    void Log(const char* message);

    // Logs GLFW + ImGui input state for one frame phase (after-poll, after-newframe, etc.).
    void LogFrame(GLFWwindow* window, const char* phase);

    void LogMouseButton(GLFWwindow* window, int button, int action);
    void LogKey(GLFWwindow* window, int key, int action);
}
