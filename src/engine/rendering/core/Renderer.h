#pragma once

struct GLFWwindow;

class Renderer
{
public:
    void SetViewport(int width, int height);
    void BeginFrame() const;
    void CancelFrame() const;
    void EndFrame(GLFWwindow* window) const;
};
