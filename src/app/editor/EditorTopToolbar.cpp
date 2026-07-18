#include "app/editor/EditorTopToolbar.h"

#include "app/core/PlayModeController.h"
#include "app/editor/EditorIcons.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "engine/platform/ui/ImGuiFonts.h"

#include <imgui.h>

#include <algorithm>

namespace
{
    float TransportButtonSize()
    {
        return ImGui::GetFrameHeight();
    }

    float MeasurePlayModeTransportWidth()
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttonSize = TransportButtonSize();
        return buttonSize * 3.0f + style.ItemSpacing.x * 2.0f;
    }

    const char* PlayStopLabel(bool playActive)
    {
        if (!ImGuiFonts::IconsAvailable())
        {
            return playActive ? "Stop" : "Play";
        }

        return playActive ? ICON_EDITOR_STOP : ICON_EDITOR_PLAY;
    }

    const char* PauseResumeLabel(bool paused)
    {
        if (!ImGuiFonts::IconsAvailable())
        {
            return paused ? "Resume" : "Pause";
        }

        return paused ? ICON_EDITOR_PLAY : ICON_EDITOR_PAUSE;
    }

    const char* StepLabel()
    {
        return ImGuiFonts::IconsAvailable() ? ICON_EDITOR_STEP : "Step";
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
    const bool startPausedArmed = !playActive && playMode.IsStartPaused();
    const std::string& projectRoot = project.GetProjectRootDirectory();
    const float transportWidth = MeasurePlayModeTransportWidth();
    const float transportX = std::max(0.0f, (ImGui::GetWindowWidth() - transportWidth) * 0.5f);
    const ImVec2 buttonSize(TransportButtonSize(), TransportButtonSize());

    ImGui::SetCursorPosX(transportX);

    if (ImGui::Button(PlayStopLabel(playActive), buttonSize))
    {
        if (!playMode.TogglePlayStop(editScene, projectRoot) && !playMode.GetLastError().empty())
        {
            project.SetStatusMessage(playMode.GetLastError());
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(playActive ? "Stop" : "Play");
    }

    ImGui::SameLine();
    if (startPausedArmed)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(PauseResumeLabel(paused), buttonSize))
    {
        playMode.TogglePause();
    }
    if (startPausedArmed)
    {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (playActive)
        {
            ImGui::SetTooltip(paused ? "Resume" : "Pause");
        }
        else if (startPausedArmed)
        {
            ImGui::SetTooltip("Start paused when entering play mode (enabled)");
        }
        else
        {
            ImGui::SetTooltip("Start paused when entering play mode");
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button(StepLabel(), buttonSize))
    {
        playMode.StepOnce();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Step one frame");
    }
    ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleVar(3);
}
