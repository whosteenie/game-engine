#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/EnvironmentPresets.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/DxrCapabilities.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/assets/FileDialog.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

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
        }
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

        ImGui::TextUnformatted("Create and edit lights from the Hierarchy and Inspector.");
    }
}
