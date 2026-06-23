#include "engine/ImGuiLayer.h"

#include "engine/ImGuiFonts.h"

#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <ImGuizmo.h>
#include <imgui_impl_opengl3.h>

#include <stdexcept>

ImGuiLayer::ImGuiLayer(GLFWwindow* window, const std::string& iniPath)
    : m_window(window), m_iniPath(iniPath)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingNoSplit = false;
    io.ConfigDockingNoDockingOver = false;
    if (!m_iniPath.empty())
    {
        io.IniFilename = m_iniPath.c_str();
    }
    else
    {
        io.IniFilename = nullptr;
    }

    ImGuiFonts::LoadEditorFonts(io);

    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true))
    {
        throw std::runtime_error("Failed to initialize ImGui GLFW backend");
    }

    if (!ImGui_ImplOpenGL3_Init("#version 330"))
    {
        throw std::runtime_error("Failed to initialize ImGui OpenGL3 backend");
    }
}

ImGuiLayer::~ImGuiLayer()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::BeginFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void ImGuiLayer::EndFrame()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
