#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/RendererSettingUi.h"
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
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawAntiAliasingSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const int viewportWidth = ctx.viewportWidth;
    const int viewportHeight = ctx.viewportHeight;
    const LightingPanelUi::FeatureState features = LightingPanelUi::QueryFeatures(renderer, screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Anti-aliasing & upscaling", true))
    {
        if (!features.postProcessingEnabled)
        {
            LightingPanelUi::DrawWrappedNote(
                "Enable Post-processing for post AA (FXAA, TAA, DLSS). Geometry MSAA is configured below.");
        }

        const AntiAliasingMode currentAaMode = screenSpaceEffects.GetAntiAliasingMode();
        const int msaaSampleCount = screenSpaceEffects.GetMsaaSampleCount();
        const int activeMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();
        const bool currentModeOwnsResolve =
            currentAaMode == AntiAliasingMode::TAA
            || currentAaMode == AntiAliasingMode::DLAA
            || currentAaMode == AntiAliasingMode::DLSS;
        const bool geometryMsaaBlocksResolve =
            msaaSampleCount > 1 || activeMsaaSampleCount > 1;
        const bool resolveBlocksGeometryMsaa = currentModeOwnsResolve;

        const DlssContext& dlss = DlssContext::Get();
        if (!dlss.IsReady())
        {
            ImGui::TextDisabled("DLSS: initializing…");
        }
        else if (dlss.IsDlssSupported())
        {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "DLSS: supported (Streamline 2.12)");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "%s", dlss.StatusString().c_str());
        }

        if (dlss.IsReady() && dlss.IsDlssSupported())
        {
            if (dlss.IsRrSupported())
            {
                ImGui::TextColored(
                    ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Ray Reconstruction: supported");
            }
            else
            {
                ImGui::TextDisabled("Ray Reconstruction: not available on this GPU/driver");
            }
        }

        const RenderDebugMode debugMode = screenSpaceEffects.GetDebugMode();
        if (debugMode != RenderDebugMode::None)
        {
            LightingPanelUi::DrawWrappedNote(
                "FXAA only affects the final image when Diagnostics > View is None.");
            ImGui::TextDisabled("Active debug view: %s", RenderDebugModeLabel(debugMode));
        }

        const bool postAaDisabled = !features.postProcessingEnabled;
        if (postAaDisabled)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::BeginCombo("Mode", AntiAliasingModeLabel(currentAaMode)))
        {
            const AntiAliasingMode selectableModes[] = {
                AntiAliasingMode::None,
                AntiAliasingMode::FXAA,
                AntiAliasingMode::SMAA,
                AntiAliasingMode::TAA,
                AntiAliasingMode::SSAA,
                AntiAliasingMode::DLAA,
                AntiAliasingMode::DLSS,
            };
            const bool dlssSupported = dlss.IsDlssSupported();
            for (const AntiAliasingMode mode : selectableModes)
            {
                const bool isDlssMode =
                    mode == AntiAliasingMode::DLAA || mode == AntiAliasingMode::DLSS;
                const bool modeOwnsResolve =
                    mode == AntiAliasingMode::TAA || isDlssMode;
                const bool disabled =
                    (geometryMsaaBlocksResolve && modeOwnsResolve)
                    || (isDlssMode && !dlssSupported);
                if (disabled)
                {
                    ImGui::BeginDisabled();
                }

                const bool selected = currentAaMode == mode;
                if (ImGui::Selectable(AntiAliasingModeLabel(mode), selected) && !selected && !disabled)
                {
                    RendererSettingUi::ApplyChange(
                        "aa_mode",
                        editContext,
                        scene,
                        "Anti-aliasing",
                        [mode](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetAntiAliasingMode(mode);
                            target.MarkDirty();
                        });
                    ImGui::CloseCurrentPopup();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                if (disabled)
                {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        if (isDlssMode && !dlssSupported)
                        {
                            ImGui::SetTooltip(
                                DlssContext::Get().IsReady()
                                    ? "Requires an NVIDIA RTX GPU with a recent driver."
                                    : "DLSS is still initializing…");
                        }
                        else if (geometryMsaaBlocksResolve && modeOwnsResolve)
                        {
                            ImGui::SetTooltip("Unavailable while geometry MSAA is enabled.");
                        }
                    }
                }
            }

            ImGui::EndCombo();
        }

        RendererSettingUi::MarkRendered("aa_mode");

        // DLSS SR quality preset (drives the internal render resolution). Enabled only in DLSS SR.
        if (currentAaMode == AntiAliasingMode::DLSS)
        {
            const DlssPreset currentPreset = screenSpaceEffects.GetDlssPreset();
            if (ImGui::BeginCombo("DLSS preset", DlssPresetLabel(currentPreset)))
            {
                const DlssPreset presets[] = {
                    DlssPreset::Quality,
                    DlssPreset::Balanced,
                    DlssPreset::Performance,
                    DlssPreset::UltraPerformance,
                };
                for (const DlssPreset preset : presets)
                {
                    const bool selected = currentPreset == preset;
                    if (ImGui::Selectable(DlssPresetLabel(preset), selected) && !selected)
                    {
                        RendererSettingUi::ApplyChange(
                            "dlss_preset",
                            editContext,
                            scene,
                            "DLSS preset",
                            [preset](Scene& target) {
                                target.GetRenderer().GetScreenSpaceEffects().SetDlssPreset(preset);
                                target.MarkDirty();
                            });
                        ImGui::CloseCurrentPopup();
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            RendererSettingUi::MarkRendered("dlss_preset");

            const int renderWidth = screenSpaceEffects.GetRenderWidth();
            const int renderHeight = screenSpaceEffects.GetRenderHeight();
            ImGui::TextDisabled(
                "Internal render: %dx%d -> display %dx%d",
                renderWidth,
                renderHeight,
                viewportWidth,
                viewportHeight);
            const float dlssGpuMs = FindGpuPassMilliseconds("DLSS");
            if (dlssGpuMs >= 0.0f)
            {
                ImGui::TextDisabled("DLSS GPU pass: %.3f ms", dlssGpuMs);
            }
            else
            {
                ImGui::TextDisabled("DLSS GPU pass: (collecting…)");
            }
        }
        else if (currentAaMode == AntiAliasingMode::DLAA)
        {
            const int renderWidth = screenSpaceEffects.GetRenderWidth();
            const int renderHeight = screenSpaceEffects.GetRenderHeight();
            ImGui::TextDisabled(
                "DLAA at native res: %dx%d",
                renderWidth,
                renderHeight);
            const float dlssGpuMs = FindGpuPassMilliseconds("DLSS");
            if (dlssGpuMs >= 0.0f)
            {
                ImGui::TextDisabled("DLSS GPU pass: %.3f ms", dlssGpuMs);
            }
        }

        const bool dlssModeActive =
            currentAaMode == AntiAliasingMode::DLAA || currentAaMode == AntiAliasingMode::DLSS;
        if (dlssModeActive)
        {
            // Ray Reconstruction (devdoc/dxr/dlss-rr.md, Phase RR5). Replaces the NRD denoisers + the
            // SR model with one neural pass. Precondition-gated: needs DLSS-RR support AND an active RT
            // feature to reconstruct. Disabled with a reason string otherwise (checkbox stays visible).
            const DxrSettings& rrDxrSettings = renderer.GetDxrSettings();
            const bool rrHasRtSignal = rrDxrSettings.IsEnabled()
                && (rrDxrSettings.IsReflectionsEnabled() || rrDxrSettings.IsShadowsEnabled()
                    || rrDxrSettings.IsGiEnabled());
            const bool rrSupported = dlss.IsReady() && dlss.IsRrSupported();
            const bool rrAvailable = rrSupported && rrHasRtSignal;
            if (!rrAvailable)
            {
                ImGui::BeginDisabled();
            }
            bool rayReconstruction = screenSpaceEffects.GetRayReconstruction();
            UndoableRendererCheckbox(
                "Ray Reconstruction",
                &rayReconstruction,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetScreenSpaceEffects().SetRayReconstruction(enabled);
                    target.MarkDirty();
                });
            RendererSettingUi::MarkRendered("ray_reconstruction");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "DLSS Ray Reconstruction: replaces the NRD denoisers AND the DLSS SR model with "
                    "one neural pass. Needs RT reflections/GI/shadows on to have a signal to reconstruct.");
            }
            if (!rrAvailable)
            {
                ImGui::EndDisabled();
                // Explain WHY it's greyed so the gate is discoverable, not mysterious.
                if (!rrSupported)
                {
                    ImGui::TextDisabled("    Requires NVIDIA RTX GPU + recent driver (DLSS 3.5).");
                }
                else
                {
                    ImGui::TextDisabled("    Enable a ray-traced feature (reflections / GI / shadows) first.");
                }
            }
            else if (screenSpaceEffects.IsRayReconstructionActive())
            {
                ImGui::TextColored(
                    ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                    "    Active - reconstructing RT signal (NRD + SR bypassed).");
            }

            // D4 (rr-gi-diagnosis.md): RR model preset A/B. DLSSDOptions are pushed every frame,
            // so Streamline hot-swaps the model on change — no restart; history resets on switch.
            {
                const bool rrPresetSelectable = rrAvailable && rayReconstruction;
                if (!rrPresetSelectable)
                {
                    ImGui::BeginDisabled();
                }
                int rrPresetIndex = static_cast<int>(screenSpaceEffects.GetRrPreset());
                const char* rrPresetLabels[] = {
                    "Default (driver)",
                    "Preset D (transformer)",
                    "Preset E (latest transformer)"};
                if (ImGui::Combo(
                        "RR model preset",
                        &rrPresetIndex,
                        rrPresetLabels,
                        IM_ARRAYSIZE(rrPresetLabels)))
                {
                    const auto preset = static_cast<DlssRrPreset>(rrPresetIndex);
                    RendererSettingUi::ApplyChange(
                        "rr_model_preset",
                        editContext,
                        scene,
                        "RR model preset",
                        [preset](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetRrPreset(preset);
                            target.MarkDirty();
                        });
                }
                if (ImGui::IsItemHovered() && rrPresetSelectable)
                {
                    ImGui::SetTooltip(
                        "Ray Reconstruction network to use (Streamline DLSSDPreset). Swaps live on the\n"
                        "next frame - expect a brief hitch while the new model loads, then a clean\n"
                        "history rebuild. A/B against GI boiling per rr-gi-diagnosis.md Phase 4 (D4).");
                }
                if (!rrPresetSelectable)
                {
                    ImGui::EndDisabled();
                }
            }
            RendererSettingUi::MarkRendered("rr_model_preset");

            float dlssSharpness = screenSpaceEffects.GetDlssSharpness();
            UndoableRendererSliderFloat(
                "DLSS sharpness",
                &dlssSharpness,
                0.0f,
                1.0f,
                "%.2f",
                editContext,
                [](Scene& target, float value) {
                    target.GetRenderer().GetScreenSpaceEffects().SetDlssSharpness(value);
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "In-DLSS sharpening (0 = off). Streamline marks this deprecated; prefer 0 for baseline quality.");
            }
        }

        if (postAaDisabled)
        {
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        if (geometryMsaaBlocksResolve || resolveBlocksGeometryMsaa)
        {
            LightingPanelUi::DrawWrappedNote(
                "Geometry MSAA above 1× and TAA/DLAA/DLSS are mutually exclusive. "
                "Incompatible options are grayed out; choosing MSAA while a resolve owner is active switches post AA to None.");
        }
        else
        {
            LightingPanelUi::DrawWrappedNote(
                "Geometry MSAA supersamples the scene pass before post AA. Pick 1× for standard single-sample rendering.");
        }

        const char* msaaPreview = "1× (None)";
        if (msaaSampleCount == 2)
        {
            msaaPreview = "2× MSAA";
        }
        else if (msaaSampleCount == 4)
        {
            msaaPreview = "4× MSAA";
        }
        else if (msaaSampleCount == 8)
        {
            msaaPreview = "8× MSAA";
        }

        static constexpr struct MsaaPreset
        {
            int count;
            const char* label;
        } kMsaaPresets[] = {
            {1, "1× (None)"},
            {2, "2× MSAA"},
            {4, "4× MSAA"},
            {8, "8× MSAA"},
        };

        if (ImGui::BeginCombo("Geometry MSAA", msaaPreview))
        {
            for (const MsaaPreset& preset : kMsaaPresets)
            {
                const bool supported = GfxContext::Get().IsMsaaSampleCountSupported(preset.count);
                const bool blockedByResolve = resolveBlocksGeometryMsaa && preset.count > 1;
                const bool disabled = !supported || blockedByResolve;
                if (disabled)
                {
                    ImGui::BeginDisabled();
                }

                const bool selected = msaaSampleCount == preset.count;
                if (ImGui::Selectable(preset.label, selected) && !selected && !disabled)
                {
                    RendererSettingUi::ApplyChange(
                        "geometry_msaa",
                        editContext,
                        scene,
                        "Geometry MSAA",
                        [preset](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetMsaaSampleCount(preset.count);
                            target.MarkDirty();
                        });
                    ImGui::CloseCurrentPopup();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                if (disabled)
                {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        if (!supported)
                        {
                            ImGui::SetTooltip("Not supported on this GPU for the scene G-buffer formats.");
                        }
                        else if (blockedByResolve)
                        {
                            ImGui::SetTooltip(
                                "Unavailable while TAA, DLAA, or DLSS owns the resolve stage. "
                                "Switch post AA to None or another mode first.");
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }
        RendererSettingUi::MarkRendered("geometry_msaa");

        const bool reloadRequested = renderer.IsGeometryMsaaReloadRequested();
        if (screenSpaceEffects.IsMsaaPendingReload())
        {
            if (reloadRequested)
            {
                ImGui::BeginDisabled();
            }
            // Requests a deferred reload; Application applies it at a safe frame boundary because
            // recreating GPU pipelines/framebuffers mid-UI can leave stale resource references and
            // crash. See SceneRenderer::RequestGeometryMsaaReload.
            if (ImGui::Button("Apply geometry MSAA") && !reloadRequested)
            {
                renderer.RequestGeometryMsaaReload();
            }
            if (reloadRequested)
            {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    "Recreates the scene renderer pipeline (framebuffer, shaders, post targets) "
                    "for the selected MSAA sample count.");
            }
            ImGui::SameLine();
            if (reloadRequested)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Applying…");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Reload required");
            }
        }
        else if (msaaSampleCount > 1)
        {
            ImGui::TextDisabled("Geometry MSAA active: %d×", msaaSampleCount);
        }

        if (renderer.HasGeometryMsaaReloadFailed() && !renderer.GetGeometryMsaaReloadError().empty())
        {
            ImGui::TextWrapped("MSAA reload failed: %s", renderer.GetGeometryMsaaReloadError().c_str());
        }

        if (currentAaMode == AntiAliasingMode::FXAA)
        {
            ImGui::TextDisabled("Tonemap -> FXAA -> viewport");
            float fxaaSubpix = screenSpaceEffects.GetFxaaSubpixQuality();
            if (ImGui::SliderFloat("FXAA subpixel quality", &fxaaSubpix, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetFxaaSubpixQuality(fxaaSubpix);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float fxaaEdge = screenSpaceEffects.GetFxaaEdgeThreshold();
            if (ImGui::SliderFloat("FXAA edge threshold", &fxaaEdge, 0.03125f, 0.5f))
            {
                screenSpaceEffects.SetFxaaEdgeThreshold(fxaaEdge);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::SMAA)
        {
            ImGui::TextDisabled("Tonemap -> SMAA edge -> SMAA blend -> viewport");
            float smaaThreshold = screenSpaceEffects.GetSmaaThreshold();
            if (ImGui::SliderFloat("SMAA edge threshold", &smaaThreshold, 0.01f, 0.25f))
            {
                screenSpaceEffects.SetSmaaThreshold(smaaThreshold);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            int smaaSteps = screenSpaceEffects.GetSmaaSearchSteps();
            if (ImGui::SliderInt("SMAA search steps", &smaaSteps, 1, 8))
            {
                screenSpaceEffects.SetSmaaSearchSteps(smaaSteps);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::TAA)
        {
            LightingPanelUi::DrawWrappedNote(
                "HDR temporal resolve (motion vectors) -> bloom -> tonemap -> viewport");
            float taaBlend = screenSpaceEffects.GetTaaBlendFactor();
            if (ImGui::SliderFloat("TAA history blend", &taaBlend, 0.0f, 0.99f))
            {
                screenSpaceEffects.SetTaaBlendFactor(taaBlend);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::SSAA)
        {
            LightingPanelUi::DrawWrappedNote("Supersampled scene -> tonemap -> downsample -> viewport");
            float renderScale = screenSpaceEffects.GetRenderScale();
            if (ImGui::SliderFloat("Render scale", &renderScale, 1.0f, 2.0f, "%.2fx"))
            {
                screenSpaceEffects.SetRenderScale(renderScale);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled(
                "Internal render: %dx%d",
                std::max(1, static_cast<int>(std::lround(static_cast<float>(viewportWidth) * renderScale))),
                std::max(1, static_cast<int>(std::lround(static_cast<float>(viewportHeight) * renderScale))));
        }
    }
}
