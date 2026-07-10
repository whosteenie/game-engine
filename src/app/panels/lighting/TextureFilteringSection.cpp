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

void DrawTextureFilteringSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;

    if (TuningSectionState::SectionHeader("Texture filtering", true))
    {
        ImGui::TextDisabled("Material textures upload with full mip chains.");

        int textureFilterMode = static_cast<int>(renderer.GetTextureFilterMode());
        const char* filterLabels[] = {"Trilinear", "Bilinear", "Nearest"};
        if (ImGui::Combo("Material sampling", &textureFilterMode, filterLabels, IM_ARRAYSIZE(filterLabels)))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture filter",
                [textureFilterMode](Scene& target) {
                    target.GetRenderer().SetTextureFilterMode(
                        static_cast<TextureFilterMode>(textureFilterMode));
                    target.MarkDirty();
                });
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Updates GfxContext for new shaders. Existing PBR shaders keep their baked samplers until restart.");
        }

        int anisotropy = static_cast<int>(renderer.GetTextureAnisotropy());
        if (ImGui::SliderInt("Anisotropic filtering", &anisotropy, 1, 16))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture anisotropy",
                [anisotropy](Scene& target) {
                    target.GetRenderer().SetTextureAnisotropy(static_cast<std::uint32_t>(anisotropy));
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);

        float mipBias = renderer.GetTextureMipBias();
        if (ImGui::SliderFloat("Mip bias", &mipBias, -2.0f, 2.0f))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture mip bias",
                [mipBias](Scene& target) {
                    target.GetRenderer().SetTextureMipBias(mipBias);
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);
    }
}
