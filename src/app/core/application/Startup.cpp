#include "app/core/Application.h"
#include "app/core/application/Detail.h"
#include "app/core/benchmark/Capture.h"
#include "app/editor/EditorSettings.h"
#include "app/project/ProjectChooser.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/platform/tooling/ProjectLoadProgress.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/ui/ImGuiLayer.h"
#include "engine/rendering/core/Renderer.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

void Application::ApplyS1p6CaptureModeIfRequested()
{
    if (m_s1p6CaptureModeApplied)
    {
        return;
    }

    const char* rawCaptureMode = std::getenv("GAME_ENGINE_S1P6_CAPTURE_MODE");
    if (rawCaptureMode == nullptr)
    {
        m_s1p6CaptureModeApplied = true;
        return;
    }
    if (!m_projectSession->HasActiveProject() || m_scene == nullptr)
    {
        return;
    }

    // Apply only after the project's deferred renderer settings. This is inert in normal runs and
    // selects existing states before the first rendered scene frame without project-file mutation.
    const std::string captureMode(rawCaptureMode);
    SceneRenderer& sceneRenderer = m_scene->GetRenderer();
    DxrSettings& dxr = sceneRenderer.GetDxrSettings();
    ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
    dxr.SetEnabled(true);
    dxr.SetRenderingMode(RenderingMode::PathTraced);
    sceneRenderer.SetRenderDebugMode(RenderDebugMode::None);

    if (captureMode == "raw-radiance")
    {
        dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
        effects.SetRayReconstruction(false);
        effects.SetAntiAliasingMode(AntiAliasingMode::None);
    }
    else if (captureMode == "rr-diffuse-guide"
        || captureMode == "rr-specular-guide"
        || captureMode == "rr-normal-roughness")
    {
        dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
        effects.SetAntiAliasingMode(AntiAliasingMode::DLAA);
        effects.SetRayReconstruction(true);
        sceneRenderer.SetRenderDebugMode(
            captureMode == "rr-diffuse-guide" ? RenderDebugMode::RrDiffuseAlbedo
            : captureMode == "rr-specular-guide" ? RenderDebugMode::RrSpecularAlbedo
                                                   : RenderDebugMode::RrNormalRoughness);
    }
    else if (captureMode == "final-rr")
    {
        dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
        effects.SetAntiAliasingMode(AntiAliasingMode::DLAA);
        effects.SetRayReconstruction(true);
    }
    else if (captureMode == "reference")
    {
        dxr.SetPtConvergenceMode(PtConvergenceMode::Reference);
        effects.SetRayReconstruction(false);
        effects.SetAntiAliasingMode(AntiAliasingMode::None);
    }
    else
    {
        throw std::runtime_error(
            "GAME_ENGINE_S1P6_CAPTURE_MODE must be raw-radiance, rr-diffuse-guide, "
            "rr-specular-guide, rr-normal-roughness, final-rr, or reference.");
    }

    if (const char* rawMirrorChainOverride =
        std::getenv("GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR"))
    {
        const std::string mirrorChainOverride(rawMirrorChainOverride);
        if (mirrorChainOverride == "0")
        {
            dxr.SetPtMirrorChainPsrEnabled(false);
        }
        else if (mirrorChainOverride == "1")
        {
            dxr.SetPtMirrorChainPsrEnabled(true);
        }
        else
        {
            throw std::runtime_error(
                "GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR must be 0 or 1.");
        }
    }
    m_s1p6CaptureModeApplied = true;
    EngineLog::Info("benchmark", "S1-P6 capture mode selected: " + captureMode);
}

void Application::ApplyS2p1CaptureModeIfRequested()
{
    if (m_s2p1CaptureModeApplied)
    {
        return;
    }

    const char* rawCaptureMode = std::getenv("GAME_ENGINE_S2P1_CAPTURE_MODE");
    const char* rawExposureEv = std::getenv("GAME_ENGINE_S2P1_EXPOSURE_EV");
    if (rawCaptureMode == nullptr && rawExposureEv == nullptr)
    {
        m_s2p1CaptureModeApplied = true;
        return;
    }
    if (rawCaptureMode == nullptr || rawExposureEv == nullptr)
    {
        throw std::runtime_error(
            "GAME_ENGINE_S2P1_CAPTURE_MODE and GAME_ENGINE_S2P1_EXPOSURE_EV must be set together.");
    }
    if (!m_projectSession->HasActiveProject() || m_scene == nullptr)
    {
        return;
    }

    const std::string captureMode(rawCaptureMode);
    const float exposureEv = std::stof(rawExposureEv);
    SceneRenderer& sceneRenderer = m_scene->GetRenderer();
    DxrSettings& dxr = sceneRenderer.GetDxrSettings();
    ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
    dxr.SetEnabled(true);
    dxr.SetRenderingMode(RenderingMode::PathTraced);
    dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
    sceneRenderer.SetRenderDebugMode(RenderDebugMode::None);
    effects.SetExposure(exposureEv);

    const bool useRr = captureMode.rfind("rr-", 0) == 0;
    const bool useDlss = captureMode.rfind("dlss-", 0) == 0;
    if (captureMode == "direct")
    {
        effects.SetAntiAliasingMode(AntiAliasingMode::None);
        effects.SetRayReconstruction(false);
    }
    else if (useDlss || useRr)
    {
        const std::string quality = captureMode.substr(captureMode.find('-') + 1);
        if (quality == "dlaa")
        {
            effects.SetAntiAliasingMode(AntiAliasingMode::DLAA);
        }
        else
        {
            effects.SetAntiAliasingMode(AntiAliasingMode::DLSS);
            effects.SetDlssPreset(
                quality == "quality" ? DlssPreset::Quality
                : quality == "balanced" ? DlssPreset::Balanced
                : quality == "performance" ? DlssPreset::Performance
                : quality == "ultra-performance" ? DlssPreset::UltraPerformance
                                                  : throw std::runtime_error(
                                                        "Invalid S2-P1 capture quality: " + quality));
        }
        effects.SetRayReconstruction(useRr);
    }
    else
    {
        throw std::runtime_error("Invalid GAME_ENGINE_S2P1_CAPTURE_MODE: " + captureMode);
    }

    m_s2p1CaptureModeApplied = true;
    EngineLog::Info(
        "benchmark",
        "S2-P1 capture mode selected: " + captureMode
            + ", authored display EV=" + std::to_string(exposureEv));
}

void Application::ApplyS2p4CaptureModeIfRequested()
{
    if (m_s2p4CaptureModeApplied)
    {
        return;
    }
    const char* rawCaptureMode = std::getenv("GAME_ENGINE_S2P4_CAPTURE_MODE");
    if (rawCaptureMode == nullptr)
    {
        m_s2p4CaptureModeApplied = true;
        return;
    }
    if (!m_projectSession->HasActiveProject() || m_scene == nullptr)
    {
        return;
    }

    const std::string captureMode(rawCaptureMode);
    SceneRenderer& sceneRenderer = m_scene->GetRenderer();
    DxrSettings& dxr = sceneRenderer.GetDxrSettings();
    ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
    dxr.SetEnabled(true);
    dxr.SetRenderingMode(RenderingMode::PathTraced);
    dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
    effects.SetAntiAliasingMode(AntiAliasingMode::DLSS);
    effects.SetDlssPreset(DlssPreset::Performance);
    effects.SetRayReconstruction(true);
    if (captureMode == "motion-vectors")
    {
        sceneRenderer.SetRenderDebugMode(RenderDebugMode::MotionVectors);
    }
    else if (captureMode == "primary-depth")
    {
        sceneRenderer.SetRenderDebugMode(RenderDebugMode::RtPrimaryDepth);
    }
    else
    {
        throw std::runtime_error(
            "GAME_ENGINE_S2P4_CAPTURE_MODE must be motion-vectors or primary-depth.");
    }
    m_s2p4CaptureModeApplied = true;
    EngineLog::Info("benchmark", "S2-P4 capture mode selected: " + captureMode);
}

bool Application::RunS2p2ExtentQueryMatrixIfRequested()
{
    if (m_s2p2ExtentQueryMatrixComplete)
    {
        return false;
    }
    const char* const outputPath = std::getenv("GAME_ENGINE_S2P2_QUERY_MATRIX_OUTPUT");
    if (outputPath == nullptr)
    {
        m_s2p2ExtentQueryMatrixComplete = true;
        return false;
    }

    DlssContext& context = DlssContext::Get();
    if (!context.IsReady())
    {
        return false;
    }

    const std::filesystem::path path(outputPath);
    std::error_code error;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error)
    {
        throw std::runtime_error("Could not create S2-P2 matrix directory: " + error.message());
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("Could not open S2-P2 matrix output: " + path.string());
    }

    constexpr DlssQuality qualities[] = {
        DlssQuality::DLAA,
        DlssQuality::Quality,
        DlssQuality::Balanced,
        DlssQuality::Performance,
        DlssQuality::UltraPerformance};
    constexpr DlssReconstructionFeature features[] = {
        DlssReconstructionFeature::SuperResolution,
        DlssReconstructionFeature::RayReconstruction};
    constexpr DlssExtent outputExtents[] = {{1280u, 720u}, {1920u, 1080u}};
    const auto qualityName = [](const DlssQuality quality) {
        switch (quality)
        {
        case DlssQuality::DLAA: return "dlaa";
        case DlssQuality::Quality: return "quality";
        case DlssQuality::Balanced: return "balanced";
        case DlssQuality::Performance: return "performance";
        case DlssQuality::UltraPerformance: return "ultra-performance";
        }
        return "unknown";
    };

    context.ClearPlannedExtentCache();
    output << "{\n  \"record_type\": \"s2p2_extent_query_matrix\",\n"
           << "  \"schema_version\": 1,\n"
           << "  \"allocation_mode\": \"s2-p4-plan-owned\",\n"
           << "  \"forced_failure\": "
           << (std::getenv("GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE") != nullptr ? "true" : "false")
           << ",\n  \"entries\": [\n";
    bool first = true;
    for (std::uint32_t viewport = 0; viewport < 2u; ++viewport)
    {
        for (const DlssReconstructionFeature feature : features)
        {
            for (const DlssQuality quality : qualities)
            {
                DlssExtentRecommendationKey key{};
                key.viewportId = viewport;
                key.outputExtent = outputExtents[viewport];
                key.feature = feature;
                key.quality = quality;
                const DlssPlannedExtent plan = context.PlanReconstructionExtent(key);
                if (!first)
                {
                    output << ",\n";
                }
                first = false;
                output << "    {\"viewport_id\": " << viewport
                       << ", \"feature\": \""
                       << (feature == DlssReconstructionFeature::RayReconstruction ? "rr" : "dlss")
                       << "\", \"quality\": \"" << qualityName(quality)
                       << "\", \"output\": [" << key.outputExtent.width << ", "
                       << key.outputExtent.height << "], \"recommended\": ["
                       << plan.extent.recommended.width << ", "
                       << plan.extent.recommended.height << "], \"minimum\": ["
                       << plan.extent.minimum.width << ", " << plan.extent.minimum.height
                       << "], \"maximum\": [" << plan.extent.maximum.width << ", "
                       << plan.extent.maximum.height << "], \"source\": \""
                       << (plan.IsSdkRecommendation() ? "sdk" : "explicit-fallback")
                       << "\", \"fallback_reason\": "
                       << (plan.fallbackReason.empty()
                               ? "null"
                               : std::string("\"") + plan.fallbackReason + "\"")
                       << ", \"rr_no_arbitrary_drs\": "
                       << (plan.rrNoArbitraryDrs ? "true" : "false") << '}';
            }
        }
    }
    output << "\n  ]\n}\n";
    output.close();
    if (!output)
    {
        throw std::runtime_error("Could not finish S2-P2 matrix output: " + path.string());
    }
    m_s2p2ExtentQueryMatrixComplete = true;
    EngineLog::Info("benchmark", "S2-P2 extent query matrix complete: " + path.string());
    return true;
}

void Application::PumpStartupFramesUntilDlssReady()
{
    if (m_window == nullptr || !GfxContext::Get().IsInitialized() || m_imguiLayer == nullptr
        || m_projectChooser == nullptr || m_projectSession == nullptr || m_scene == nullptr
        || m_editorSettings == nullptr || m_renderer == nullptr)
    {
        return;
    }

    constexpr int kMaxBootstrapFrames = 600;
    for (int frameIndex = 0;
         frameIndex < kMaxBootstrapFrames && (frameIndex == 0 || !DlssContext::Get().IsReady())
         && !glfwWindowShouldClose(m_window);
         ++frameIndex)
    {
        glfwPollEvents();
        UpdatePendingProjectStartupProgress("Finishing graphics initialization...");

        m_imguiLayer->BeginFrame();
        m_projectChooser->Draw(
            *m_projectSession,
            *m_scene,
            *m_editorSettings,
            m_projectEditorState,
            [](const ProjectEditorState&) {},
            [this]() { RequestClose(); },
            []() {},
            m_undoStack,
            m_editorClipboard);
        m_renderer->BeginFrame();
        m_imguiLayer->EndFrame();
        m_renderer->EndFrame(m_window);

        if (frameIndex == 0)
        {
#ifdef _WIN32
            // The first swapchain image now completely replaces the lightweight GDI startup
            // surface. Restore GLFW's window procedure only after that present succeeds.
            ApplicationDetail::DetachStartupWindowPaint(m_window);
#endif
        }
    }

    GfxContext::Get().TryDeferredStreamlineSwapChainUpgrade();
}


