#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/Application.h"
#include "app/EditorSettings.h"
#include "app/LightingPanel.h"
#include "app/MainMenuBar.h"
#include "app/ProjectChooser.h"
#include "app/ProjectFilesPanel.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"
#include "app/SceneEditor.h"
#include "app/SceneHierarchyPanel.h"
#include "app/SceneInspectorPanel.h"
#include "app/SceneToolbarPanel.h"
#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/FileDialog.h"
#include "engine/ImGuiLayer.h"
#include "engine/NativeProgressWindow.h"
#include "engine/Input.h"
#include "engine/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <stdexcept>

#include <glm/glm.hpp>

namespace
{
    bool TrySaveProject(ProjectSession& project, Scene& scene)
    {
        if (!project.IsUntitled())
        {
            return project.Save(scene);
        }

        std::string projectPath;
        if (!FileDialog::SaveProjectFile(projectPath, project.GetProjectFilePath()))
        {
            return false;
        }

        return project.SaveAs(scene, projectPath);
    }
}

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
    m_editorSettings = std::make_unique<EditorSettings>();
    m_editorSettings->Load();
    m_projectSession = std::make_unique<ProjectSession>();
    m_projectChooser = std::make_unique<ProjectChooser>();
    m_mainMenuBar = std::make_unique<MainMenuBar>();
    m_lightingPanel = std::make_unique<LightingPanel>();
    m_sceneToolbarPanel = std::make_unique<SceneToolbarPanel>();
    m_sceneHierarchyPanel = std::make_unique<SceneHierarchyPanel>();
    m_sceneInspectorPanel = std::make_unique<SceneInspectorPanel>();
    m_projectFilesPanel = std::make_unique<ProjectFilesPanel>();
    m_camera = std::make_unique<Camera>(
        glm::vec3(6.0f, 5.0f, 6.0f),
        -135.0f,
        -35.0f);

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    OnFramebufferResize(framebufferWidth, framebufferHeight);

    m_input = std::make_unique<Input>(m_window);
    m_scene = std::make_unique<Scene>();
    m_scene->SetDirtyCallback([this]() { m_projectSession->MarkDirty(); });

    glfwSetCursorPosCallback(m_window, MouseCallback);
    glfwSetWindowCloseCallback(m_window, WindowCloseCallback);
}

Application::~Application()
{
    NativeProgressWindow::Instance().Shutdown();
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
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

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

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();

    m_projectChooser->Draw(
        *m_projectSession,
        *m_scene,
        *m_editorSettings,
        [this]() { RequestClose(); });

    if (editorActive)
    {
        EditorPanelVisibility panelVisibility;
        panelVisibility.hierarchy = &m_sceneHierarchyPanel->ShowPanel();
        panelVisibility.inspector = &m_sceneInspectorPanel->ShowPanel();
        panelVisibility.toolbar = &m_sceneToolbarPanel->ShowPanel();
        panelVisibility.lighting = &m_lightingPanel->ShowPanel();
        panelVisibility.project = &m_projectFilesPanel->ShowPanel();

        m_mainMenuBar->Draw(
            *m_scene,
            *m_projectSession,
            *m_editorSettings,
            m_window,
            panelVisibility,
            [this]() { RequestClose(); },
            [this]() { RequestNewProject(); });

        m_sceneToolbarPanel->Draw(*m_scene);
        m_sceneHierarchyPanel->Draw(*m_scene, *m_projectSession);
        m_sceneInspectorPanel->Draw(*m_scene);
        m_projectFilesPanel->Draw(*m_projectSession);
        m_lightingPanel->Draw(*m_scene, *m_camera);
    }

    DrawUnsavedChangesDialog();

    const ImGuiIO& io = ImGui::GetIO();

    m_input->UpdateMouseCapture(!io.WantCaptureMouse);

    const bool flyCameraActive = m_input->IsCapturingMouse();
    if (io.WantCaptureMouse && !flyCameraActive)
    {
        m_input->ReleaseMouseCapture();
    }

    const bool allowGameKeyboard = !io.WantCaptureKeyboard || flyCameraActive;
    const bool allowGameMouse = flyCameraActive || !io.WantCaptureMouse;
    const bool allowSceneMouse = editorActive && !flyCameraActive && allowGameMouse;

    if (allowGameKeyboard && m_input->WasKeyPressed(GLFW_KEY_ESCAPE))
    {
        RequestClose();
    }

    if (editorActive && allowGameMouse && flyCameraActive)
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

    if (editorActive)
    {
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
    }

    m_input->EndFrame();
}

void Application::RequestClose()
{
    if (!m_projectSession->HasActiveProject() || !m_projectSession->IsDirty())
    {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        return;
    }

    m_pendingClose = true;
}

void Application::RequestNewProject()
{
    if (m_projectSession->IsDirty())
    {
        m_pendingNewProject = true;
        return;
    }

    m_projectChooser->OpenNewProjectForm();
}

void Application::DrawUnsavedChangesDialog()
{
    if (!m_pendingClose && !m_pendingNewProject)
    {
        return;
    }

    const bool isClosePrompt = m_pendingClose;
    ImGui::OpenPopup("Unsaved Changes");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(
            "Unsaved Changes",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    }

    if (isClosePrompt)
    {
        ImGui::TextUnformatted("Save changes before closing?");
    }
    else
    {
        ImGui::TextUnformatted("Save changes before creating a new project?");
    }

    ImGui::Separator();

    if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
    {
        if (TrySaveProject(*m_projectSession, *m_scene))
        {
            if (isClosePrompt)
            {
                m_pendingClose = false;
                ImGui::CloseCurrentPopup();
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            else
            {
                m_pendingNewProject = false;
                ImGui::CloseCurrentPopup();
                m_projectSession->CloseProject();
                m_projectChooser->OpenNewProjectForm();
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(120.0f, 0.0f)))
    {
        if (isClosePrompt)
        {
            m_pendingClose = false;
            ImGui::CloseCurrentPopup();
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        }
        else
        {
            m_pendingNewProject = false;
            ImGui::CloseCurrentPopup();
            m_projectSession->CloseProject();
            m_projectChooser->OpenNewProjectForm();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
    {
        m_pendingClose = false;
        m_pendingNewProject = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void Application::WindowCloseCallback(GLFWwindow* window)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    app->RequestClose();
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

    const bool editorActive =
        m_projectSession->HasActiveProject() && !m_projectChooser->IsBlockingEditor();
    if (editorActive)
    {
        m_scene->Render(*m_camera, viewportWidth, viewportHeight);
    }

    m_imguiLayer->EndFrame();
    m_renderer->EndFrame(m_window);
}

void Application::MouseCallback(GLFWwindow* window, double xPos, double yPos)
{
    ImGui_ImplGlfw_CursorPosCallback(window, xPos, yPos);

    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    app->m_input->OnMouseMove(xPos, yPos);
}
