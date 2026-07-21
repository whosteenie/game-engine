#include "engine/platform/input/InputDiagnostics.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    bool g_enabled = false;
    bool g_enabledInitialized = false;
    std::uint64_t g_frameCounter = 0;

    bool QueryEnabled()
    {
        if (!g_enabledInitialized)
        {
            g_enabledInitialized = true;
            const char* value = std::getenv("GAME_ENGINE_INPUT_DEBUG");
            g_enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        }

        return g_enabled;
    }
}

namespace InputDiagnostics
{
    bool IsEnabled()
    {
        return QueryEnabled();
    }

    void Log(const char* message)
    {
        if (!QueryEnabled() || message == nullptr)
        {
            return;
        }

        std::fprintf(stderr, "[input] %s\n", message);
        std::fflush(stderr);
    }

    void LogFrame(GLFWwindow* window, const char* phase)
    {
        if (!QueryEnabled())
        {
            return;
        }

        if (phase != nullptr && std::strcmp(phase, "after-poll") == 0)
        {
            ++g_frameCounter;
        }

        double cursorX = 0.0;
        double cursorY = 0.0;
        if (window != nullptr)
        {
            glfwGetCursorPos(window, &cursorX, &cursorY);
        }

        const ImGuiIO& io = ImGui::GetIO();
        const int focused = window != nullptr ? glfwGetWindowAttrib(window, GLFW_FOCUSED) : 0;
        const int hovered = window != nullptr ? glfwGetWindowAttrib(window, GLFW_HOVERED) : 0;
        const int iconified = window != nullptr ? glfwGetWindowAttrib(window, GLFW_ICONIFIED) : 0;

        std::fprintf(
            stderr,
            "[input] frame=%llu phase=%s focused=%d hovered=%d iconified=%d "
            "glfwCursor=(%.1f,%.1f) imguiMouse=(%.1f,%.1f) imguiDown=L%d R%d "
            "wantCaptureMouse=%d wantCaptureKeyboard=%d displaySize=(%.0f,%.0f)\n",
            static_cast<unsigned long long>(g_frameCounter),
            phase != nullptr ? phase : "?",
            focused,
            hovered,
            iconified,
            cursorX,
            cursorY,
            io.MousePos.x,
            io.MousePos.y,
            io.MouseDown[0] ? 1 : 0,
            io.MouseDown[1] ? 1 : 0,
            io.WantCaptureMouse ? 1 : 0,
            io.WantCaptureKeyboard ? 1 : 0,
            io.DisplaySize.x,
            io.DisplaySize.y);
        std::fflush(stderr);
    }

    void LogMouseButton(GLFWwindow* window, int button, int action)
    {
        if (!QueryEnabled())
        {
            return;
        }

        double cursorX = 0.0;
        double cursorY = 0.0;
        if (window != nullptr)
        {
            glfwGetCursorPos(window, &cursorX, &cursorY);
        }

        std::fprintf(
            stderr,
            "[input] mouse-button button=%d action=%d cursor=(%.1f,%.1f)\n",
            button,
            action,
            cursorX,
            cursorY);
        std::fflush(stderr);
    }

    void LogKey(GLFWwindow* window, int key, int action)
    {
        if (!QueryEnabled())
        {
            return;
        }

        (void)window;
        std::fprintf(stderr, "[input] key key=%d action=%d\n", key, action);
        std::fflush(stderr);
    }
}
