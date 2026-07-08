#include "app/panels/lighting/LightingPanelUi.h"

#include "app/scene/SceneRenderer.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"

#include <imgui.h>

namespace LightingPanelUi
{
    TextWrapScope::TextWrapScope()
    {
        const float wrapWidth = ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x;
        if (wrapWidth > 0.0f)
        {
            ImGui::PushTextWrapPos(wrapWidth);
            m_active = true;
        }
    }

    TextWrapScope::~TextWrapScope()
    {
        if (m_active)
        {
            ImGui::PopTextWrapPos();
        }
    }

    void DrawWrappedNote(const char* text)
    {
        const TextWrapScope wrap;
        ImGui::TextDisabled("%s", text);
    }

    void DrawWrappedHelp(const char* text)
    {
        const TextWrapScope wrap;
        ImGui::TextWrapped("%s", text);
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
