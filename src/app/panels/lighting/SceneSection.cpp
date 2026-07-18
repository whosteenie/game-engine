#include "app/panels/lighting/LightingPanelSections.h"
#include "app/editor/EditorSettings.h"
#include "app/editor/RendererSettingUi.h"
#include "app/editor/TuningSectionState.h"
#include "engine/rhi/GfxContext.h"

#include <imgui.h>


void DrawSceneSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;

    if (TuningSectionState::SectionHeader("Scene", true))
    {
        bool vsyncEnabled = GfxContext::Get().IsVsyncEnabled();
        if (ImGui::Checkbox("Vertical sync", &vsyncEnabled))
        {
            GfxContext::Get().SetVsyncEnabled(vsyncEnabled);
            if (ctx.editorSettings != nullptr)
            {
                ctx.editorSettings->SetVsyncEnabled(vsyncEnabled);
                ctx.editorSettings->Save();
            }
        }
        RendererSettingUi::MarkRendered("vsync");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Synchronizes presentation to the display refresh rate. Disable to test lower input latency; "
                "your display driver controls G-Sync/FreeSync.");
        }

        bool showGizmos = scene.GetShowLightGizmos();
        if (ImGui::Checkbox("Show light gizmos", &showGizmos))
        {
            if (editContext.undoStack != nullptr)
            {
                PushSceneEditorViewMutation(
                    *editContext.undoStack,
                    scene,
                    "Light gizmos",
                    [&](Scene& target) {
                        target.SetShowLightGizmos(showGizmos);
                    });
            }
            else
            {
                scene.SetShowLightGizmos(showGizmos);
            }
        }
        RendererSettingUi::MarkRendered("show_light_gizmos");

        ImGui::TextUnformatted("Create and edit lights from the Hierarchy and Inspector.");
    }
}
