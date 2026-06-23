#include "app/EditorTopToolbar.h"

#include "app/PlayModeController.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"

#include <imgui.h>

#include <algorithm>

namespace
{
    float MeasureTransportButtonWidth(const char* label)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        return textSize.x + style.FramePadding.x * 2.0f;
    }

    float MeasurePlayModeTransportWidth(const PlayModeController& playMode)
    {
        const bool playActive = playMode.IsActive();
        const bool paused = playMode.GetState() == PlayModeState::Paused;
        const ImGuiStyle& style = ImGui::GetStyle();

        const float playButtonWidth = MeasureTransportButtonWidth(playActive ? "Stop" : "Play");
        const float pauseButtonWidth = MeasureTransportButtonWidth(paused ? "Resume" : "Pause");
        float transportWidth = playButtonWidth + style.ItemSpacing.x + pauseButtonWidth;

        if (playActive)
        {
            const char* stateLabel = paused ? "| Paused" : "| Playing";
            transportWidth += style.ItemSpacing.x + ImGui::CalcTextSize(stateLabel).x;
        }

        return transportWidth;
    }
}

void EditorTopToolbar::Draw(
    PlayModeController& playMode,
    Scene& editScene,
    ProjectSession& project)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& defaultStyle = ImGui::GetStyle();

    constexpr float kToolbarFramePaddingY = 6.0f;
    constexpr float kToolbarWindowPaddingY = 4.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(defaultStyle.FramePadding.x, kToolbarFramePaddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, kToolbarWindowPaddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const float toolbarHeight = ImGui::GetFrameHeight() + kToolbarWindowPaddingY * 2.0f;
    m_height = toolbarHeight;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags toolbarFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (!ImGui::Begin("##EditorTopToolbar", nullptr, toolbarFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    const bool playActive = playMode.IsActive();
    const bool paused = playMode.GetState() == PlayModeState::Paused;
    const std::string& projectRoot = project.GetProjectRootDirectory();
    const float transportWidth = MeasurePlayModeTransportWidth(playMode);
    const float transportX = std::max(0.0f, (ImGui::GetWindowWidth() - transportWidth) * 0.5f);

    ImGui::SetCursorPosX(transportX);

    if (ImGui::Button(
            playActive ? "Stop" : "Play",
            ImVec2(MeasureTransportButtonWidth(playActive ? "Stop" : "Play"), 0.0f)))
    {
        if (!playMode.TogglePlayStop(editScene, projectRoot) && !playMode.GetLastError().empty())
        {
            project.SetStatusMessage(playMode.GetLastError());
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!playActive);
    if (ImGui::Button(
            paused ? "Resume" : "Pause",
            ImVec2(MeasureTransportButtonWidth(paused ? "Resume" : "Pause"), 0.0f)))
    {
        playMode.TogglePause();
    }
    ImGui::EndDisabled();

    if (playActive)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(paused ? "| Paused" : "| Playing");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}
