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
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawRayTracingSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;

    if (TuningSectionState::SectionHeader("Ray tracing", true))
    {
        const GfxContext& gfx = GfxContext::Get();
        const bool raytracingSupported = gfx.IsInitialized() && gfx.IsRaytracingSupported();
        const int raytracingTier = gfx.IsInitialized() ? gfx.GetRaytracingTier() : 0;
        const std::string& adapterName =
            gfx.IsInitialized() ? gfx.GetAdapterDescription() : std::string("(GPU not initialized)");

        ImGui::Text("Adapter: %s", adapterName.c_str());
        char tierText[64]{};
        FormatRaytracingTierText(raytracingTier, tierText, sizeof(tierText));
        ImGui::Text("Ray tracing tier: %s", tierText);

        if (!raytracingSupported)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                "Ray tracing requires a DXR Tier 1.0+ GPU and up-to-date driver.");
            if (raytracingTier == 0 && gfx.IsInitialized())
            {
                LightingPanelUi::DrawWrappedNote(
                    "Update your graphics driver (NVIDIA 531+ class recommended) if you expect RTX support.");
            }
        }

        DxrSettings& dxrSettings = renderer.GetDxrSettings();

        // RR5: when Ray Reconstruction is running it replaces the NRD denoisers entirely, so their
        // tuning controls are inert. Render them disabled with a reason (not hidden) — toggling RR off
        // brings them back live. The RT feature enables + trace params stay live (RR consumes them).
        const bool rrActive = screenSpaceEffects.IsRayReconstructionActive();

        if (!raytracingSupported)
        {
            ImGui::BeginDisabled();
        }

        ImGui::PushID("RayTracing");
        bool dxrEnabled = dxrSettings.IsEnabled();
        const bool pathTracingActive =
            dxrEnabled && dxrSettings.GetRenderingMode() == RenderingMode::PathTraced;
        const bool ptRrSupported = DlssContext::Get().IsReady() && DlssContext::Get().IsRrSupported();
        UndoableRendererCheckbox(
            "Enable ray tracing",
            &dxrEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                EngineLog::Breadcrumb(
                    "dxr",
                    std::string("editor: Enable ray tracing -> ") + (enabled ? "on" : "off"));
                if (enabled)
                {
                    ResetDxrBreadcrumbOnceFlags();
                }
                target.GetRenderer().GetDxrSettings().SetEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.GetRenderer().GetScreenSpaceEffects().InvalidateSsrHistory();
                target.MarkDirty();
            });

        // Phase P0 — rendering mode (devdoc/dxr/path-tracing.md). Hybrid (raster + hybrid RT, the
        // default) vs the unified path tracer. Path tracing needs master RT on; greyed otherwise.
        const bool renderingModeSelectable = dxrEnabled;
        if (!renderingModeSelectable)
        {
            ImGui::BeginDisabled();
        }
        int renderingModeIndex = static_cast<int>(dxrSettings.GetRenderingMode());
        const char* renderingModeLabels[] = {"Hybrid (raster + RT)", "Path traced"};
        if (ImGui::Combo(
                "Rendering mode",
                &renderingModeIndex,
                renderingModeLabels,
                IM_ARRAYSIZE(renderingModeLabels)))
        {
            const auto mode = static_cast<RenderingMode>(renderingModeIndex);
            ApplyRendererChange(
                editContext,
                scene,
                "Rendering mode",
                [mode](Scene& target) {
                    target.GetRenderer().GetDxrSettings().SetRenderingMode(mode);
                    if (mode == RenderingMode::PathTraced && !GfxContext::Get().IsFrameRecording())
                    {
                        target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                    }
                    target.MarkDirty();
                });
        }
        if (ImGui::IsItemHovered() && renderingModeSelectable)
        {
            ImGui::SetTooltip(
                "Path traced: the unified path tracer owns the image (P0 shows primary-hit normals).\n"
                "Hybrid keeps the raster + RT pipeline. Additive - both are selectable for comparison.");
        }
        if (!renderingModeSelectable)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("    Enable ray tracing to use path tracing.");
        }

        if (dxrSettings.GetRenderingMode() == RenderingMode::PathTraced && dxrEnabled)
        {
            const bool rrConvergenceSelectable = ptRrSupported;
            if (!rrConvergenceSelectable)
            {
                ImGui::BeginDisabled();
            }
            int convergenceModeIndex = static_cast<int>(dxrSettings.GetPtConvergenceMode());
            const char* convergenceModeLabels[] = {"Real-time (DLSS-RR)", "Reference (accumulate)"};
            if (ImGui::Combo(
                    "PT convergence",
                    &convergenceModeIndex,
                    convergenceModeLabels,
                    IM_ARRAYSIZE(convergenceModeLabels)))
            {
                const auto mode = static_cast<PtConvergenceMode>(convergenceModeIndex);
                if (mode == PtConvergenceMode::RealTime && !ptRrSupported)
                {
                    // Combo can still be changed programmatically; keep Reference when RR is absent.
                }
                else
                {
                    ApplyRendererChange(
                        editContext,
                        scene,
                        "PT convergence mode",
                        [mode](Scene& target) {
                            target.GetRenderer().GetDxrSettings().SetPtConvergenceMode(mode);
                            target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                            target.MarkDirty();
                        });
                }
            }
            if (!rrConvergenceSelectable)
            {
                ImGui::EndDisabled();
                ImGui::TextDisabled("    Real-time needs DLSS Ray Reconstruction on this GPU.");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Reference: progressive HDR accumulation while the camera and scene are static.\n"
                    "Resets on camera move, resize, light/scene edits, or setting changes.\n"
                    "Real-time: 1 spp path trace denoised via DLSS Ray Reconstruction.");
            }

            // Diagnostic switchboard (devdoc/dxr/pt/gi-shimmer.md): which RR inputs come from the
            // PT vs raster. Direct set, no undo — this is a debug control, not scene state.
            int rrBundleMode = dxrSettings.GetPtRrBundleMode();
            const char* rrBundleLabels[] = {
                "Full PT (depth+motion+guides)",
                "Raster bundle (stable fallback)",
                "PT guides only",
                "PT depth+motion",
                "PT depth only",
                "PT motion only"};
            if (ImGui::Combo(
                    "RR inputs (debug)",
                    &rrBundleMode,
                    rrBundleLabels,
                    IM_ARRAYSIZE(rrBundleLabels)))
            {
                dxrSettings.SetPtRrBundleMode(rrBundleMode);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Shimmer isolation: pick which DLSS-RR inputs come from the path tracer vs the\n"
                    "raster G-buffer. 'Raster bundle' is the previous stable configuration.\n"
                    "Full PT uses merged motion (raster geometry + PT sky); modes 3/5 use raw PT\n"
                    "motion for diagnosis only.");
            }

            int ptMaxBounces = dxrSettings.GetPtMaxBounces();
            UndoableRendererSliderInt(
                "PT max bounces",
                &ptMaxBounces,
                1,
                16,
                editContext,
                [](Scene& target, int bounces) {
                    target.GetRenderer().GetDxrSettings().SetPtMaxBounces(bounces);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });

            bool ptRussianRoulette = dxrSettings.IsPtRussianRouletteEnabled();
            UndoableRendererCheckbox(
                "PT Russian roulette",
                &ptRussianRoulette,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetPtRussianRouletteEnabled(enabled);
                    target.MarkDirty();
                });

            bool ptFireflyClamp = dxrSettings.IsPtFireflyClampEnabled();
            UndoableRendererCheckbox(
                "PT firefly clamp",
                &ptFireflyClamp,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetPtFireflyClampEnabled(enabled);
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Clamps rare ultra-bright path samples before denoise/accumulate.\n"
                    "Slightly biased; turn off in Reference for ground truth.");
            }

            float ptAmbientStrength = dxrSettings.GetPtAmbientStrength();
            UndoableRendererSliderFloat(
                "PT ambient strength",
                &ptAmbientStrength,
                0.0f,
                2.0f,
                "%.2f",
                editContext,
                [](Scene& target, float strength) {
                    target.GetRenderer().GetDxrSettings().SetPtAmbientStrength(strength);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Real-time only: scales the AO-gated SH sky ambient at the primary hit.\n"
                    "Independent of sun intensity. 0 = no ambient floor (crevices go black).");
            }

            int ptAmbientAoRayCount = dxrSettings.GetPtAmbientAoRayCount();
            UndoableRendererSliderInt(
                "PT ambient AO rays",
                &ptAmbientAoRayCount,
                0,
                8,
                editContext,
                [](Scene& target, int rays) {
                    target.GetRenderer().GetDxrSettings().SetPtAmbientAoRayCount(rays);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Real-time only: short cosine visibility rays that darken SH ambient in crevices.\n"
                    "0 = unoccluded ambient (recommended). Raise only if open shadows wash out.");
            }

            float ptSunAngularRadius = dxrSettings.GetSunAngularRadiusDegrees();
            UndoableRendererSliderFloat(
                "PT sun angular radius",
                &ptSunAngularRadius,
                0.05f,
                2.0f,
                "%.2f deg",
                editContext,
                [](Scene& target, float degrees) {
                    target.GetRenderer().GetDxrSettings().SetSunAngularRadiusDegrees(degrees);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Angular radius of the sun disk for soft shadow penumbra (real-time PT NEE).\n"
                    "~0.27 deg matches the real sun. Wider = softer contact shadows. Shared with RT shadows.");
            }

            if (dxrSettings.IsPtReferenceConvergence())
            {
                const std::uint32_t spp =
                    scene.GetRenderer().GetScreenSpaceEffects().GetPathTracerAccumSampleCount();
                if (spp >= 64u)
                {
                    ImGui::TextDisabled("    Reference: %u spp (converged)", spp);
                }
                else if (spp > 0u)
                {
                    ImGui::TextDisabled("    Reference: %u spp (converging...)", spp);
                }
                else
                {
                    ImGui::TextDisabled("    Reference: accumulating...");
                }
            }
            else
            {
                if (screenSpaceEffects.PathTracerResolvedViaDlssThisFrame())
                {
                    ImGui::TextDisabled("    Real-time: DLSS-RR reconstructed this frame");
                }
                else if (ptRrSupported)
                {
                    ImGui::TextDisabled("    Real-time: awaiting DLSS-RR...");
                }
                const float pathTracerMs = FindGpuPassMilliseconds("Path tracer");
                if (pathTracerMs >= 0.0f)
                {
                    ImGui::TextDisabled("    Path tracer GPU: %.3f ms", pathTracerMs);
                }
                const std::uint32_t statsSamples =
                    screenSpaceEffects.GetPathTracerTemporalStatsSampleCount();
                if (screenSpaceEffects.IsPathTracerBoilMetricValid())
                {
                    ImGui::TextDisabled(
                        "    PT boil metric: %.5f (%u samples)",
                        screenSpaceEffects.GetPathTracerBoilMetric(),
                        statsSamples);
                }
                else if (statsSamples > 0u)
                {
                    ImGui::TextDisabled("    PT boil metric: pending (%u samples)", statsSamples);
                }
            }
        }

        bool debugTraceEnabled = dxrSettings.IsDebugTraceEnabled();
        UndoableRendererCheckbox(
            "Enable RT debug trace",
            &debugTraceEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDebugTraceEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });

        if (pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote("Hybrid RT effects are handled by the path tracer.");
            ImGui::BeginDisabled();
        }

        bool reflectionsEnabled = dxrSettings.IsReflectionsEnabled();
        UndoableRendererCheckbox(
            "Enable RT reflections",
            &reflectionsEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetReflectionsEnabled(enabled);
                target.MarkDirty();
            });

        int qualityIndex = static_cast<int>(dxrSettings.GetReflectionsQuality());
        const char* qualityLabels[] = {"Low", "Medium", "High"};
        if (ImGui::Combo("Reflections quality", &qualityIndex, qualityLabels, IM_ARRAYSIZE(qualityLabels)))
        {
            const auto quality = static_cast<DxrReflectionsQuality>(qualityIndex);
            ApplyRendererChange(
                editContext,
                scene,
                "RT reflections quality",
                [quality](Scene& target) {
                    target.GetRenderer().GetDxrSettings().SetReflectionsQuality(quality);
                    target.MarkDirty();
                });
        }

        int samplesPerPixel = dxrSettings.GetReflectionsSamplesPerPixel();
        UndoableRendererSliderInt(
            "Reflection samples / pixel",
            &samplesPerPixel,
            1,
            16,
            editContext,
            [](Scene& target, int samples) {
                target.GetRenderer().GetDxrSettings().SetReflectionsSamplesPerPixel(samples);
                target.MarkDirty();
            });

        float maxTraceDistance = dxrSettings.GetMaxTraceDistance();
        UndoableRendererSliderFloat(
            "Max trace distance",
            &maxTraceDistance,
            1.0f,
            500.0f,
            "%.1f m",
            editContext,
            [](Scene& target, float distance) {
                target.GetRenderer().GetDxrSettings().SetMaxTraceDistance(distance);
                target.MarkDirty();
            });

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("Reflection denoise (NRD RELAX): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool denoiseEnabled = dxrSettings.IsDenoiseEnabled();
        UndoableRendererCheckbox(
            "Denoise enabled",
            &denoiseEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDenoiseEnabled(enabled);
                target.MarkDirty();
            });

        float temporalBlend = dxrSettings.GetTemporalBlend();
        UndoableRendererSliderFloat(
            "Temporal blend",
            &temporalBlend,
            0.0f,
            0.99f,
            "%.2f",
            editContext,
            [](Scene& target, float blend) {
                target.GetRenderer().GetDxrSettings().SetTemporalBlend(blend);
                target.MarkDirty();
            });

        int atrousIterations = dxrSettings.GetReflectionAtrousIterations();
        UndoableRendererSliderInt(
            "Denoiser smoothing (A-trous)",
            &atrousIterations,
            2,
            8,
            editContext,
            [](Scene& target, int iterations) {
                target.GetRenderer().GetDxrSettings().SetReflectionAtrousIterations(iterations);
                target.MarkDirty();
            });

        bool antiFirefly = dxrSettings.IsReflectionAntiFireflyEnabled();
        UndoableRendererCheckbox(
            "Denoiser anti-firefly",
            &antiFirefly,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetReflectionAntiFireflyEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        int aoRays = dxrSettings.GetReflectionAoRays();
        UndoableRendererSliderInt(
            "Reflection AO rays",
            &aoRays,
            0,
            16,
            editContext,
            [](Scene& target, int rays) {
                target.GetRenderer().GetDxrSettings().SetReflectionAoRays(rays);
                target.MarkDirty();
            });
        LightingPanelUi::DrawWrappedNote(
            "Contact shadows on reflected surfaces. 0 = off; higher = cleaner, costlier.");

        float roughnessCutoff = dxrSettings.GetReflectionRoughnessCutoff();
        UndoableRendererSliderFloat(
            "Reflection roughness cutoff",
            &roughnessCutoff,
            0.0f,
            1.0f,
            "%.2f",
            editContext,
            [](Scene& target, float cutoff) {
                target.GetRenderer().GetDxrSettings().SetReflectionRoughnessCutoff(cutoff);
                target.MarkDirty();
            });
        LightingPanelUi::DrawWrappedNote(
            "Surfaces rougher than this skip the RT trace and use IBL (cheaper, less blur).");

        // Phase D8 — RT soft sun shadows (devdoc/dxr/shadows.md). Supplemental over CSM.
        ImGui::SeparatorText("RT shadows");

        bool shadowsEnabled = dxrSettings.IsShadowsEnabled();
        UndoableRendererCheckbox(
            "Enable RT shadows",
            &shadowsEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetShadowsEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });

        float sunAngularRadius = dxrSettings.GetSunAngularRadiusDegrees();
        UndoableRendererSliderFloat(
            "Sun angular radius",
            &sunAngularRadius,
            0.05f,
            2.0f,
            "%.2f deg",
            editContext,
            [](Scene& target, float degrees) {
                target.GetRenderer().GetDxrSettings().SetSunAngularRadiusDegrees(degrees);
                target.MarkDirty();
            });
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Angular radius of the sun disk. Drives RT shadow penumbra and path-traced soft sun NEE.");
        }

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("Shadow denoise (NRD SIGMA): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool shadowDenoise = dxrSettings.IsShadowDenoiseEnabled();
        UndoableRendererCheckbox(
            "Shadow denoise (SIGMA)",
            &shadowDenoise,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetShadowDenoiseEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        // Phase D9 — RT diffuse GI. Mutually exclusive with SSGI inject.
        ImGui::SeparatorText("RT diffuse GI");

        const bool ssgiBlocksRtGi = screenSpaceEffects.IsSsgiEnabled();
        if (ssgiBlocksRtGi)
        {
            LightingPanelUi::DrawWrappedNote("SSGI is enabled. Disable SSGI before enabling RT diffuse GI.");
        }

        if (ssgiBlocksRtGi)
        {
            ImGui::BeginDisabled();
        }

        bool giEnabled = dxrSettings.IsGiEnabled();
        UndoableRendererCheckbox(
            "Enable RT GI",
            &giEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetGiEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });
        if (ssgiBlocksRtGi)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Mutually exclusive with SSGI inject.");
            }
        }

        float giStrength = dxrSettings.GetGiStrength();
        UndoableRendererSliderFloat(
            "GI strength",
            &giStrength,
            0.0f,
            2.0f,
            "%.2f",
            editContext,
            [](Scene& target, float strength) {
                target.GetRenderer().GetDxrSettings().SetGiStrength(strength);
                target.MarkDirty();
            });

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("GI denoise (NRD RELAX): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool giDenoise = dxrSettings.IsGiDenoiseEnabled();
        UndoableRendererCheckbox(
            "GI denoise (RELAX)",
            &giDenoise,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetGiDenoiseEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        if (pathTracingActive)
        {
            ImGui::EndDisabled();
        }

        LightingPanelUi::DrawWrappedNote(
            "Additive over ambient. Lower Environment intensity if the scene washes out. Overrides SSGI when enabled.");

        ImGui::PopID();

        const DxrDiagnostics& dxrDiagnostics = renderer.GetDxrDiagnostics();
        ImGui::Separator();
        ImGui::Text("BLAS count: %u", dxrDiagnostics.blasCount);
        ImGui::Text("TLAS instances: %u", dxrDiagnostics.tlasInstanceCount);
        ImGui::Text("RT triangles (unique): %llu", static_cast<unsigned long long>(dxrDiagnostics.totalRtTriangles));
        const double asMemoryMb =
            static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / (1024.0 * 1024.0);
        if (asMemoryMb >= 1.0)
        {
            ImGui::Text("AS GPU memory: %.2f MB", asMemoryMb);
        }
        else
        {
            ImGui::Text(
                "AS GPU memory: %.1f KB",
                static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / 1024.0);
        }
        ImGui::Text("Last build status: %s", dxrDiagnostics.buildStatus.c_str());
        ImGui::Text("Last build time: %.3f ms", dxrDiagnostics.lastBuildTimeMs);
        ImGui::Text(
            "Emissive NEE lights: %u  (tris: %u)",
            dxrDiagnostics.emissiveLightCount,
            dxrDiagnostics.emissiveTriangleCount);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Emitters registered for path-tracer emissive NEE (refreshed each PT frame).\n"
                "0 = NEE toward emitters is OFF, so the 'PT isolate: emissive NEE' view is\n"
                "legitimately black and emitter light only arrives via random BSDF hits.\n"
                "Non-zero = NEE is active; a dark isolate view just means few visible surfaces\n"
                "directly face the emitter.");
        }

        if (!raytracingSupported)
        {
            ImGui::EndDisabled();
        }

        LightingPanelUi::DrawWrappedNote(
            "Acceleration structures build when ray tracing is enabled. DispatchRays arrives in a later DXR phase.");
    }
}
