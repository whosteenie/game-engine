#include "app/panels/lighting/LightingPanelUi.h"

#include "app/editor/EditorWidgets.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/rendering/core/DxrSettings.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

#include <imgui.h>

namespace LightingPanelUi
{
    void DrawWrappedNote(const char* text)
    {
        EditorWidgets::TextWrappedDisabled(text);
    }

    void DrawWrappedHelp(const char* text)
    {
        const EditorWidgets::TextWrapScope wrap;
        ImGui::TextWrapped("%s", text);
    }

    void DrawTooltipForLastItem(const char* text)
    {
        if (!ImGui::IsItemHovered() || text == nullptr || *text == '\0')
        {
            return;
        }

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    FeatureState QueryFeatures(const SceneRenderer& renderer, const ScreenSpaceEffects& screenSpaceEffects)
    {
        FeatureState state;
        state.postProcessingEnabled = screenSpaceEffects.IsEnabled();
        const DxrSettings& dxrSettings = renderer.GetDxrSettings();
        state.dxrEnabled = dxrSettings.IsEnabled();
        state.pathTracingActive = dxrSettings.IsPathTracingActive();
        state.rtGiEnabled = state.dxrEnabled && dxrSettings.IsGiEnabled();
        state.ssgiEnabled = screenSpaceEffects.IsSsgiEnabled();
        state.rayReconstructionActive = screenSpaceEffects.IsRayReconstructionActive();
        state.debugViewActive = screenSpaceEffects.GetDebugMode() != RenderDebugMode::None;
        return state;
    }
}
