#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "Application.h"
#include "Mesh.h"
#include "Shader.h"

#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

Application::Application(int width, int height, const char* title)
    : m_width(width), m_height(height), m_title(title)
{
    InitGLFW();
    InitGLAD();

    glfwSwapInterval(1);
    glEnable(GL_DEPTH_TEST);

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    OnFramebufferResize(framebufferWidth, framebufferHeight);

    m_shader = new Shader("assets/shaders/triangle.vert", "assets/shaders/triangle.frag");

    const float k = 0.4330127f;
    float vertices[] = {
         0.0f,  0.5f, 0.0f,
        -k,    -0.25f, 0.0f,
         k,    -0.25f, 0.0f
    };
    m_mesh = new Mesh(vertices, 3);
}

Application::~Application()
{
    delete m_mesh;
    delete m_shader;

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
    HandleInput(deltaTime);

    if (!m_paused)
    {
        m_animationTime += deltaTime;
    }
}

void Application::HandleInput(double deltaTime)
{
    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(m_window, true);
    }

    bool spaceDown = glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (spaceDown && !m_spaceWasDown)
    {
        m_paused = !m_paused;
    }
    m_spaceWasDown = spaceDown;

    const float moveSpeed = 2.0f;
    if (glfwGetKey(m_window, GLFW_KEY_LEFT) == GLFW_PRESS)
    {
        m_position.x -= moveSpeed * static_cast<float>(deltaTime);
    }
    if (glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS)
    {
        m_position.x += moveSpeed * static_cast<float>(deltaTime);
    }
    if (glfwGetKey(m_window, GLFW_KEY_UP) == GLFW_PRESS)
    {
        m_position.y += moveSpeed * static_cast<float>(deltaTime);
    }
    if (glfwGetKey(m_window, GLFW_KEY_DOWN) == GLFW_PRESS)
    {
        m_position.y -= moveSpeed * static_cast<float>(deltaTime);
    }
}

void Application::OnFramebufferResize(int width, int height)
{
    glViewport(0, 0, width, height);

    if (width > 0 && height > 0)
    {
        m_aspect = static_cast<float>(height) / static_cast<float>(width);
    }
}

void Application::FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->OnFramebufferResize(width, height);
}

void Application::Render()
{
    glClearColor(0.08f, 0.09f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_shader->Use();
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, m_position);
    model = glm::rotate(model, (float)m_animationTime * 1.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, (float)m_animationTime * 0.6f, glm::vec3(1.0f, 0.0f, 0.0f));

    glm::mat4 view = glm::lookAt(
        glm::vec3(0.0f, 0.0f, 3.0f),   // camera position
        glm::vec3(0.0f, 0.0f, 0.0f),   // look-at target
        glm::vec3(0.0f, 1.0f, 0.0f)    // up direction
    );
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        1.0f / m_aspect,
        0.1f,
        100.0f
    );
    
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", projection);
    m_shader->SetFloat("uTime", static_cast<float>(m_animationTime));
    m_mesh->Draw();

    glfwSwapBuffers(m_window);
}
