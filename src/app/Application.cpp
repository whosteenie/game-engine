#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/Application.h"
#include "app/DemoScene.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/Input.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/Renderer.h"

#include <stdexcept>

#include <glm/glm.hpp>

Application::Application(int width, int height, const char* title)
    : m_width(width), m_height(height), m_title(title)
{
    InitGLFW();
    InitGLAD();

    glfwSetCursorPosCallback(m_window, MouseCallback);

    glfwSwapInterval(1);
    glEnable(GL_DEPTH_TEST);

    m_renderer = std::make_unique<Renderer>();
    m_camera = std::make_unique<Camera>(
        glm::vec3(6.0f, 5.0f, 6.0f),
        -135.0f,
        -35.0f);
    m_light = std::make_unique<Light>(glm::vec3(4.0f, 6.0f, 3.0f));
    m_material = std::make_unique<Material>(
        EngineConstants::PhongVertexShader,
        EngineConstants::PhongFragmentShader,
        glm::vec3(0.9f, 0.15f, 0.1f));

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    OnFramebufferResize(framebufferWidth, framebufferHeight);

    m_input = std::make_unique<Input>(m_window);
    m_scene = std::make_unique<DemoScene>();
}

Application::~Application()
{
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void Application::Run()
{
    double lastFrameTime = glfwGetTime();

    while (!glfwWindowShouldClose(m_window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        Update(deltaTime);
        Render();
    }
}

void Application::InitGLFW()
{
    if (!glfwInit())
    {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_window = glfwCreateWindow(m_width, m_height, m_title, nullptr, nullptr);
    if (!m_window)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_window);
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, FramebufferSizeCallback);
}

void Application::InitGLAD()
{
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        throw std::runtime_error("Failed to initialize GLAD");
    }
}

void Application::Update(double deltaTime)
{
    glfwPollEvents();

    m_input->UpdateMouseCapture();

    if (m_input->IsKeyDown(GLFW_KEY_ESCAPE))
    {
        glfwSetWindowShouldClose(m_window, true);
    }

    if (m_input->WasKeyPressed(GLFW_KEY_SPACE))
    {
        m_paused = !m_paused;
    }

    m_camera->ProcessKeyboard(*m_input, static_cast<float>(deltaTime));

    if (m_input->IsCapturingMouse())
    {
        m_camera->ProcessMouseMovement(
            m_input->ConsumeMouseDeltaX(),
            m_input->ConsumeMouseDeltaY());
    }
    else
    {
        m_input->ConsumeMouseDeltaX();
        m_input->ConsumeMouseDeltaY();
    }

    m_scene->Update(deltaTime, m_paused, *m_input);
}

void Application::OnFramebufferResize(int width, int height)
{
    m_renderer->SetViewport(width, height);
    m_camera->SetAspectFromFramebuffer(width, height);
}

void Application::FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->OnFramebufferResize(width, height);
}

void Application::Render()
{
    m_renderer->BeginFrame();
    m_scene->Render(*m_camera, *m_light, *m_material);
    m_renderer->EndFrame(m_window);
}

void Application::MouseCallback(GLFWwindow* window, double xPos, double yPos)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->m_input->OnMouseMove(xPos, yPos);
}