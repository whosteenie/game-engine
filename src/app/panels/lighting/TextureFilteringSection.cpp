#include "app/panels/lighting/LightingPanelSections.h"
#include "app/editor/RendererSettingUi.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/rhi/GfxContext.h"

#include <imgui.h>

#include <cstdint>


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
            RendererSettingUi::ApplyChange(
                "texture_filter_mode",
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
        RendererSettingUi::MarkRendered("texture_filter_mode");

        int anisotropy = static_cast<int>(renderer.GetTextureAnisotropy());
        if (ImGui::SliderInt("Anisotropic filtering", &anisotropy, 1, 16))
        {
            RendererSettingUi::ApplyChange(
                "texture_anisotropy",
                editContext,
                scene,
                "Texture anisotropy",
                [anisotropy](Scene& target) {
                    target.GetRenderer().SetTextureAnisotropy(static_cast<std::uint32_t>(anisotropy));
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);
        RendererSettingUi::MarkRendered("texture_anisotropy");

        float mipBias = renderer.GetTextureMipBias();
        if (ImGui::SliderFloat("Mip bias", &mipBias, -2.0f, 2.0f))
        {
            RendererSettingUi::ApplyChange(
                "texture_mip_bias",
                editContext,
                scene,
                "Texture mip bias",
                [mipBias](Scene& target) {
                    target.GetRenderer().SetTextureMipBias(mipBias);
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);
        RendererSettingUi::MarkRendered("texture_mip_bias");
        LightingPanelUi::DrawTooltipForLastItem(
            "Chooses sharper or blurrier texture detail levels. Negative values sharpen but can add shimmer.");
    }
}
