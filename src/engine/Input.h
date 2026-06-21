#pragma once

struct GLFWwindow;

#include <unordered_map>

class Input
{
public:
    explicit Input(GLFWwindow* window);

    void UpdateMouseCapture();
    void ReleaseMouseCapture();

    bool IsKeyDown(int key) const;
    bool WasKeyPressed(int key);
    bool IsMouseButtonDown(int button) const;
    bool IsCapturingMouse() const;

    void OnMouseMove(double xPos, double yPos);
    float ConsumeMouseDeltaX();
    float ConsumeMouseDeltaY();

private:
    void SetRawMouseMotion(bool enabled);

    GLFWwindow* m_window;
    std::unordered_map<int, bool> m_previousKeyState;

    bool m_capturingMouse = false;
    bool m_firstMouse = true;
    double m_lastMouseX = 0.0;
    double m_lastMouseY = 0.0;
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
};
