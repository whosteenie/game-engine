#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/Application.h"
#include "app/DebugPanel.h"
#include "app/Scene.h"
#include "app/SceneEditor.h"
#include "app/SceneHierarchyPanel.h"
#include "app/SceneInspectorPanel.h"
#include "app/SceneToolbarPanel.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/ImGuiLayer.h"
#include "engine/Input.h"
#include "engine/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <stdexcept>

#include <glm/glm.hpp>

Application::Application(int width, int height, const char* title)
    : m_width(width), m_height(height), m_title(title)
{
    InitGLFW();
    InitGLAD();

    glfwSwapInterval(1);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    m_renderer = std::make_unique<Renderer>();
    m_imguiLayer = std::make_unique<ImGuiLayer>(m_window);
    m_debugPanel = std::make_unique<DebugPanel>();
    m_sceneToolbarPanel = std::make_unique<SceneToolbarPanel>();
    m_sceneHierarchyPanel = std::make_unique<SceneHierarchyPanel>();
    m_sceneInspectorPanel = std::make_unique<SceneInspectorPanel>();
    m_camera = std::make_unique<Camera>(
        glm::vec3(6.0f, 5.0f, 6.0f),
        -135.0f,
        -35.0f);

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    OnFramebufferResize(framebufferWidth, framebufferHeight);

    m_input = std::make_unique<Input>(m_window);
    m_scene = std::make_unique<Scene>();

    glfwSetCursorPosCallback(m_window, MouseCallback);
}

Application::~Application()
{
    m_imguiLayer.reset();
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
    glfwWindowHint(GLFW_SAMPLES, 4);

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

    m_imguiLayer->BeginFrame();
    m_sceneToolbarPanel->Draw(*m_scene);
    m_sceneHierarchyPanel->Draw(*m_scene);
    m_sceneInspectorPanel->Draw(*m_scene);
    m_debugPanel->Draw(*m_scene, *m_camera);

    const ImGuiIO& io = ImGui::GetIO();

    m_input->UpdateMouseCapture(!io.WantCaptureMouse);

    const bool flyCameraActive = m_input->IsCapturingMouse();
    if (io.WantCaptureMouse && !flyCameraActive)
    {
        m_input->ReleaseMouseCapture();
    }

    const bool allowGameKeyboard = !io.WantCaptureKeyboard || flyCameraActive;
    const bool allowGameMouse = flyCameraActive || !io.WantCaptureMouse;
    const bool allowSceneMouse = !flyCameraActive && allowGameMouse;

    if (allowGameKeyboard && m_input->WasKeyPressed(GLFW_KEY_ESCAPE))
    {
        glfwSetWindowShouldClose(m_window, true);
    }

    if (allowGameMouse && flyCameraActive)
    {
        m_camera->ProcessKeyboard(*m_input, static_cast<float>(deltaTime));
        m_camera->ProcessMouseMovement(
            m_input->ConsumeMouseDeltaX(),
            m_input->ConsumeMouseDeltaY());
    }
    else
    {
        m_input->ConsumeMouseDeltaX();
        m_input->ConsumeMouseDeltaY();
    }

    int viewportWidth = 0;
    int viewportHeight = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetFramebufferSize(m_window, &viewportWidth, &viewportHeight);
    glfwGetWindowSize(m_window, &windowWidth, &windowHeight);

    m_scene->Update(
        *m_input,
        *m_camera,
        viewportWidth,
        viewportHeight,
        windowWidth,
        windowHeight,
        allowSceneMouse,
        allowGameKeyboard);

    m_input->EndFrame();
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
    int viewportWidth = 0;
    int viewportHeight = 0;
    glfwGetFramebufferSize(m_window, &viewportWidth, &viewportHeight);

    m_renderer->BeginFrame();
    m_scene->Render(*m_camera, viewportWidth, viewportHeight);
    m_imguiLayer->EndFrame();
    m_renderer->EndFrame(m_window);
}

void Application::MouseCallback(GLFWwindow* window, double xPos, double yPos)
{
    ImGui_ImplGlfw_CursorPosCallback(window, xPos, yPos);

    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->m_input->OnMouseMove(xPos, yPos);
}
