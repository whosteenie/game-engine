#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/Input.h"

Input::Input(GLFWwindow* window)
    : m_window(window)
{
}

void Input::UpdateMouseCapture()
{
    bool rightMouseDown = IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);

    if (rightMouseDown && !m_capturingMouse)
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
        m_capturingMouse = false;
        m_mouseDeltaX = 0.0f;
        m_mouseDeltaY = 0.0f;
        SetRawMouseMotion(false);
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
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
