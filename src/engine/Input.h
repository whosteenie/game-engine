#pragma once

struct GLFWwindow;

#include <unordered_map>

class Input
{
public:
    explicit Input(GLFWwindow* window);

    bool IsKeyDown(int key) const;
    bool WasKeyPressed(int key);

private:
    GLFWwindow* m_window;
    std::unordered_map<int, bool> m_previousKeyState;
};
