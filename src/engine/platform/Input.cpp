#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/platform/Input.h"

#include <glm/glm.hpp>

Input::Input(GLFWwindow* window)
    : m_window(window)
{
}

void Input::UpdateMouseCapture(bool allowStartCapture)
{
    const bool rightMouseDown = IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);

    if (rightMouseDown && !m_capturingMouse && allowStartCapture)
    {
        m_capturingMouse = true;
        m_firstMouse = true;
        m_mouseDeltaX = 0.0f;
        m_mouseDeltaY = 0.0f;
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        SetRawMouseMotion(true);
    }
    else if (!rightMouseDown && m_capturingMouse)
    {
        ReleaseMouseCapture();
    }
}

void Input::ReleaseMouseCapture()
{
    if (!m_capturingMouse)
    {
        return;
    }

    m_capturingMouse = false;
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
    SetRawMouseMotion(false);
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void Input::SetRawMouseMotion(bool enabled)
{
    if (glfwRawMouseMotionSupported())
    {
        glfwSetInputMode(
            m_window,
            GLFW_RAW_MOUSE_MOTION,
            enabled ? GLFW_TRUE : GLFW_FALSE);
    }
}

bool Input::IsKeyDown(int key) const
{
    return glfwGetKey(m_window, key) == GLFW_PRESS;
}

bool Input::WasKeyPressed(int key)
{
    bool down = IsKeyDown(key);
    bool wasDown = m_previousKeyState[key];
    m_previousKeyState[key] = down;
    return down && !wasDown;
}

bool Input::IsMouseButtonDown(int button) const
{
    return glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

bool Input::IsCapturingMouse() const
{
    return m_capturingMouse;
}

void Input::OnMouseMove(double xPos, double yPos)
{
    if (!m_capturingMouse)
    {
        return;
    }

    if (m_firstMouse)
    {
        m_lastMouseX = xPos;
        m_lastMouseY = yPos;
        m_firstMouse = false;
        return;
    }

    m_mouseDeltaX += static_cast<float>(xPos - m_lastMouseX);
    m_mouseDeltaY += static_cast<float>(m_lastMouseY - yPos);

    m_lastMouseX = xPos;
    m_lastMouseY = yPos;
}

float Input::ConsumeMouseDeltaX()
{
    float delta = m_mouseDeltaX;
    m_mouseDeltaX = 0.0f;
    return delta;
}

float Input::ConsumeMouseDeltaY()
{
    float delta = m_mouseDeltaY;
    m_mouseDeltaY = 0.0f;
    return delta;
}

bool Input::WasMouseButtonPressed(int button)
{
    const bool down = IsMouseButtonDown(button);
    const bool wasDown = m_previousMouseButtonState[button];
    return down && !wasDown;
}

bool Input::WasMouseButtonReleased(int button)
{
    const bool down = IsMouseButtonDown(button);
    const bool wasDown = m_previousMouseButtonState[button];
    return !down && wasDown;
}

void Input::GetCursorPosition(double& x, double& y) const
{
    glfwGetCursorPos(m_window, &x, &y);
}

glm::vec2 Input::GetCursorPositionFramebufferScaled(
    int framebufferWidth,
    int framebufferHeight,
    int windowWidth,
    int windowHeight) const
{
    double cursorX = 0.0;
    double cursorY = 0.0;
    GetCursorPosition(cursorX, cursorY);

    const float scaleX = windowWidth > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth) : 1.0f;
    const float scaleY = windowHeight > 0 ? static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight) : 1.0f;
    return glm::vec2(static_cast<float>(cursorX) * scaleX, static_cast<float>(cursorY) * scaleY);
}

void Input::EndFrame()
{
    for (int button = GLFW_MOUSE_BUTTON_1; button <= GLFW_MOUSE_BUTTON_8; ++button)
    {
        m_previousMouseButtonState[button] = IsMouseButtonDown(button);
    }
}
