#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/rendering/Renderer.h"

void Renderer::SetViewport(int width, int height)
{
    glViewport(0, 0, width, height);
}

void Renderer::BeginFrame() const
{
    glClearColor(0.08f, 0.09f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::CancelFrame() const
{
}

void Renderer::EndFrame(GLFWwindow* window) const
{
    glfwSwapBuffers(window);
}
