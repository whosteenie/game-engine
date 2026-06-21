#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/Input.h"

Input::Input(GLFWwindow* window)
    : m_window(window)
{
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
