#include "app/core/Application.h"
#include "app/core/application/Detail.h"
#include "app/core/benchmark/Capture.h"
#include "app/editor/EditorDockSpace.h"
#include "app/project/ProjectChooser.h"
#include "app/project/ProjectSession.h"
#include "app/scene/document/Scene.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/input/InputDiagnostics.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/ui/ImGuiLayer.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rhi/GfxContext.h"
#include "engine/camera/Camera.h"
#include "engine/rendering/core/Renderer.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    std::string DescribeException(const std::exception& exception)
    {
        return SafeExceptionMessage(exception);
    }
}

void Application::Run()
{
    m_automatedBenchmarkCapture = AutomatedBenchmarkCapture::CreateFromEnvironment();
    m_automatedOpticalOrbitCapture = AutomatedOpticalOrbitCapture::CreateFromEnvironment();
    ProjectLoadBenchmark::StartFromEnvironment();
    const bool s2p5Matrix = std::getenv("GAME_ENGINE_S2P5_MODE_MATRIX") != nullptr;
    m_automationDualViewportLayout =
        std::getenv("GAME_ENGINE_AUTOMATION_DUAL_VIEW") != nullptr || s2p5Matrix;
    if (m_automationDualViewportLayout)
    {
        // S2-P5 deliberately uses unequal visible Scene/Game extents. The ordinary automation
        // layout remains an even split for all earlier diagnostics.
        m_editorDockSpace->SetAutomationDualViewportLayout(true, s2p5Matrix ? 0.38f : 0.5f);
    }

    if (const char* autoOpenPath = std::getenv("GAME_ENGINE_AUTO_OPEN"))
    {
        std::string error;
        ProjectLoadBenchmark::Mark("project.auto_open.begin");
        if (std::getenv("GAME_ENGINE_AUTO_OPEN_DEFERRED") != nullptr)
        {
            m_projectChooser->QueueProjectOpen(autoOpenPath);
        }
        else if (!m_projectChooser->OpenProjectAtPath(
                     *m_projectSession,
                     *m_scene,
                     *m_editorSettings,
                     m_projectEditorState,
                     autoOpenPath,
                     [this](const ProjectEditorState& editorState) { ApplyProjectEditorState(editorState); },
                     m_undoStack,
                     m_editorClipboard,
                     [this]() { ResetEditorLayoutLoadState(); },
                     error))
        {
            m_projectChooser->SetErrorMessage(error.empty() ? "Failed to open project." : error);
            ProjectLoadBenchmark::Fail(error.empty() ? "Failed to open project." : error);
            RequestForcedClose();
        }
        else
        {
            ProjectLoadBenchmark::Mark("project.auto_open.complete");
        }
    }

    if (const char* rawProbe = std::getenv("GAME_ENGINE_BENCHMARK_PT_PROBE"))
    {
        try
        {
            const RenderDebugMode mode = RenderDebugModeFromPtDebugIsolateMode(std::stoi(rawProbe));
            if (mode == RenderDebugMode::None)
            {
                throw std::runtime_error("must be one of 28, 29, or 30");
            }
            m_scene->GetRenderer().SetRenderDebugMode(mode);
            EngineLog::Info("benchmark", "Enabled PT environment-DI performance probe " + std::string(rawProbe));
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("GAME_ENGINE_BENCHMARK_PT_PROBE must be 28, 29, or 30.");
        }
    }

    double lastFrameTime = glfwGetTime();
    std::string lastLoggedFrameError;
    int suppressedRepeatedFrameErrors = 0;
    const bool s0p3Transitions = std::getenv("GAME_ENGINE_S0P3_TRANSITIONS") != nullptr;
    std::uint64_t s0p3TransitionFrame = 0;
    const bool s1p4Transitions = std::getenv("GAME_ENGINE_S1P4_TRANSITIONS") != nullptr;
    std::uint64_t s1p4TransitionFrame = 0;
    const bool s1p4DualOwnership =
        std::getenv("GAME_ENGINE_S1P4_DUAL_OWNERSHIP") != nullptr;
    const bool s2p4Transitions = std::getenv("GAME_ENGINE_S2P4_TRANSITIONS") != nullptr;
    std::uint64_t s2p4TransitionFrame = 0;
    const bool s2p4FallbackSmoke = std::getenv("GAME_ENGINE_S2P4_FALLBACK_SMOKE") != nullptr;
    std::uint64_t s2p4FallbackFrame = 0;
    std::uint64_t s2p5ReadyFrame = 0;
    std::uint64_t s2p5MatrixFrame = 0;
    const char* s2p4CaptureMode = std::getenv("GAME_ENGINE_S2P4_CAPTURE_MODE");
    const bool s2p4MotionAov = s2p4CaptureMode != nullptr
        && std::string_view(s2p4CaptureMode) == "motion-vectors";
    const bool autoReopenOnce = std::getenv("GAME_ENGINE_AUTO_REOPEN_ONCE") != nullptr;
    int autoReopenState = 0;
    int autoReopenReadyFrames = 0;
    std::string autoReopenPath;

    while (!glfwWindowShouldClose(m_window))
    {
        if (ApplicationDetail::ConsumeConsoleCloseRequest())
        {
            RequestForcedClose();
            break;
        }
        if (RunS2p2ExtentQueryMatrixIfRequested())
        {
            RequestForcedClose();
            break;
        }

        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;
        const auto frameWorkStart = std::chrono::steady_clock::now();
        ApplicationFrameDiagnostics frameDiagnostics{};

        // Opt-in lifecycle benchmark: allow a project to settle, return through the normal
        // project teardown boundary, then reopen it in the same process. AutomatedBenchmarkCapture
        // is gated until the reopen completes so its frame window measures the menu lifecycle.
        if (autoReopenOnce)
        {
            if (autoReopenState == 0
                && m_projectSession->HasActiveProject()
                && !m_projectChooser->IsBlockingEditor()
                && m_scene->GetRenderer().IsGpuResourcesReady())
            {
                if (++autoReopenReadyFrames >= 120)
                {
                    autoReopenPath = m_projectSession->GetProjectFilePath();
                    m_undoStack.Clear();
                    m_editorClipboard.Clear();
                    m_projectSession->CloseProject();
                    m_pendingProjectTeardown = true;
                    autoReopenState = 1;
                }
            }
            else if (autoReopenState == 1 && !m_pendingProjectTeardown)
            {
                m_projectChooser->QueueProjectOpen(autoReopenPath);
                autoReopenState = 2;
            }
            else if (autoReopenState == 2
                && m_projectSession->HasActiveProject()
                && !m_projectChooser->IsBlockingEditor()
                && m_scene->GetRenderer().IsGpuResourcesReady()
                && !m_projectChooser->IsPresentingProjectLoad())
            {
                autoReopenState = 3;
            }
        }

        // S0-P3 capture-only transition matrix. This calls the existing UI setters and GLFW resize
        // path; it is inert unless explicitly enabled by the diagnostic capture script.
        if (s0p3Transitions && m_projectSession->HasActiveProject() && m_scene != nullptr)
        {
            ++s0p3TransitionFrame;
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
            switch (s0p3TransitionFrame)
            {
            case 30: effects.SetRayReconstruction(false); break;
            case 60: effects.SetRayReconstruction(true); break;
            case 90: effects.SetAntiAliasingMode(AntiAliasingMode::DLSS); break;
            case 120: effects.SetDlssPreset(DlssPreset::Performance); break;
            case 150: glfwSetWindowSize(m_window, std::max(640, m_width - 160), std::max(480, m_height - 90)); break;
            case 180: sceneRenderer.SetRenderDebugMode(RenderDebugMode::PtRestirGiSpatialStaticVariance); break;
            case 210: sceneRenderer.SetRenderDebugMode(RenderDebugMode::None); break;
            case 240: sceneRenderer.GetDxrSettings().SetRenderingMode(RenderingMode::Hybrid); break;
            case 270: sceneRenderer.GetDxrSettings().SetRenderingMode(RenderingMode::PathTraced); break;
            default: break;
            }
        }

        // S1-P4 dual ownership capture stays on raster/no-AA so a narrow automation pane never
        // reaches Streamline. It is inert outside the dedicated evidence process.
        if (s1p4DualOwnership && m_projectSession->HasActiveProject() && m_scene != nullptr)
        {
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            sceneRenderer.GetDxrSettings().SetEnabled(false);
            sceneRenderer.GetScreenSpaceEffects().SetAntiAliasingMode(AntiAliasingMode::None);
        }

        // Keep the camera moving during the capture-only motion AOV run so the presented
        // diagnostic contains measurable vectors instead of a valid but uniformly black image.
        if (s2p4MotionAov && m_camera != nullptr && m_projectSession->HasActiveProject()
            && m_scene != nullptr && m_scene->GetRenderer().IsGpuResourcesReady())
        {
            m_camera->SetPosition(m_camera->GetPosition() + glm::vec3(0.0025f, 0.0f, 0.0f));
        }

        // S1-P4 capture-only compatibility matrix. Production behavior is unchanged unless the
        // dedicated evidence script opts in.
        if (s1p4Transitions && m_projectSession->HasActiveProject() && m_scene != nullptr)
        {
            ++s1p4TransitionFrame;
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
            DxrSettings& dxr = sceneRenderer.GetDxrSettings();
            switch (s1p4TransitionFrame)
            {
            case 10: dxr.SetEnabled(false); break;
            case 20:
                dxr.SetEnabled(true);
                dxr.SetRenderingMode(RenderingMode::Hybrid);
                break;
            case 30: dxr.SetRenderingMode(RenderingMode::PathTraced); break;
            case 40: effects.SetRayReconstruction(false); break;
            case 50: effects.SetRayReconstruction(true); break;
            case 60: effects.SetAntiAliasingMode(AntiAliasingMode::DLAA); break;
            case 70:
                effects.SetAntiAliasingMode(AntiAliasingMode::DLSS);
                effects.SetDlssPreset(DlssPreset::Performance);
                break;
            case 80: dxr.SetPtRrBundleMode(3); break;
            case 90: dxr.SetPtRrBundleMode(0); break;
            case 110:
                if (m_camera != nullptr)
                {
                    m_camera->SetPosition(m_camera->GetPosition() + glm::vec3(3.0f, 0.0f, 0.0f));
                }
                break;
            case 120: effects.InvalidateMotionHistory(); break;
            case 130: dxr.SetRenderingMode(RenderingMode::Hybrid); break;
            case 140: dxr.SetRenderingMode(RenderingMode::PathTraced); break;
            default: break;
            }
        }

        // S2-P4 capture-only active allocation/tag matrix. It uses the production setters and one
        // controlled windowed aspect resize; it is inert outside the dedicated evidence process.
        if (s2p4Transitions && m_projectSession->HasActiveProject() && m_scene != nullptr
            && m_scene->GetRenderer().IsGpuResourcesReady())
        {
            ++s2p4TransitionFrame;
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
            DxrSettings& dxr = sceneRenderer.GetDxrSettings();
            const auto selectTuple = [&](const bool rr, const AntiAliasingMode aa, const DlssPreset preset)
            {
                effects.SetRayReconstruction(rr);
                effects.SetAntiAliasingMode(aa);
                if (aa == AntiAliasingMode::DLSS)
                {
                    effects.SetDlssPreset(preset);
                }
                const char* quality = aa == AntiAliasingMode::DLAA ? "dlaa"
                    : preset == DlssPreset::Quality ? "quality"
                    : preset == DlssPreset::Balanced ? "balanced"
                    : preset == DlssPreset::Performance ? "performance"
                    : "ultra-performance";
                EngineLog::Info(
                    "benchmark",
                    std::string("S2-P4 tuple selected feature=") + (rr ? "rr" : "dlss")
                        + " quality=" + quality);
            };
            switch (s2p4TransitionFrame)
            {
            case 1:
                dxr.SetEnabled(true);
                dxr.SetRenderingMode(RenderingMode::PathTraced);
                sceneRenderer.SetRenderDebugMode(RenderDebugMode::None);
                selectTuple(false, AntiAliasingMode::DLAA, DlssPreset::Quality);
                break;
            case 20: selectTuple(false, AntiAliasingMode::DLSS, DlssPreset::Quality); break;
            case 40: selectTuple(false, AntiAliasingMode::DLSS, DlssPreset::Balanced); break;
            case 60: selectTuple(false, AntiAliasingMode::DLSS, DlssPreset::Performance); break;
            case 80: selectTuple(false, AntiAliasingMode::DLSS, DlssPreset::UltraPerformance); break;
            case 100: selectTuple(true, AntiAliasingMode::DLAA, DlssPreset::Quality); break;
            case 120: selectTuple(true, AntiAliasingMode::DLSS, DlssPreset::Quality); break;
            case 140: selectTuple(true, AntiAliasingMode::DLSS, DlssPreset::Balanced); break;
            case 160: selectTuple(true, AntiAliasingMode::DLSS, DlssPreset::Performance); break;
            case 180: selectTuple(true, AntiAliasingMode::DLSS, DlssPreset::UltraPerformance); break;
            case 220:
                // 1280x720 -> 1440x900 changes both size and aspect while remaining a safe,
                // non-minimized viewport. The resize stabilizer commits one coherent reallocation.
                glfwSetWindowSize(m_window, 1440, 900);
                EngineLog::Info("benchmark", "S2-P4 resize selected window=1440x900");
                selectTuple(false, AntiAliasingMode::DLSS, DlssPreset::Quality);
                break;
            case 260: selectTuple(true, AntiAliasingMode::DLSS, DlssPreset::Performance); break;
            case 300:
                sceneRenderer.SetRenderDebugMode(RenderDebugMode::MotionVectors);
                EngineLog::Info("benchmark", "S2-P4 AOV selected motion-vectors");
                break;
            case 420:
                sceneRenderer.SetRenderDebugMode(RenderDebugMode::RtPrimaryDepth);
                EngineLog::Info("benchmark", "S2-P4 AOV selected primary-depth");
                break;
            case 500: selectTuple(false, AntiAliasingMode::DLAA, DlssPreset::Quality); break;
            case 540:
                sceneRenderer.SetRenderDebugMode(RenderDebugMode::None);
                EngineLog::Info("benchmark", "S2-P4 AOV selected final");
                break;
            case 580: RequestForcedClose(); break;
            default: break;
            }
        }

        // S2-P4 explicit-query-failure smoke: prove ordinary SR fallback and native RR fallback
        // both own real allocations/tags without weakening the production failure visibility.
        if (s2p4FallbackSmoke && m_projectSession->HasActiveProject() && m_scene != nullptr
            && m_scene->GetRenderer().IsGpuResourcesReady())
        {
            ++s2p4FallbackFrame;
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
            DxrSettings& dxr = sceneRenderer.GetDxrSettings();
            switch (s2p4FallbackFrame)
            {
            case 1:
                dxr.SetEnabled(true);
                dxr.SetRenderingMode(RenderingMode::PathTraced);
                effects.SetRayReconstruction(false);
                effects.SetAntiAliasingMode(AntiAliasingMode::DLSS);
                effects.SetDlssPreset(DlssPreset::Performance);
                EngineLog::Info("benchmark", "S2-P4 fallback selected feature=dlss quality=performance");
                break;
            case 60:
                effects.SetRayReconstruction(true);
                EngineLog::Info("benchmark", "S2-P4 fallback selected feature=rr quality=performance");
                break;
            case 120: RequestForcedClose(); break;
            default: break;
            }
        }

        // S2-P5 capture-only cross-system gate. It changes no production invariant: every cell
        // uses the same setters as the editor, while the evidence script validates the resulting
        // history, jitter, allocation, Streamline, exposure, and D3D12 records.
        if (s2p5Matrix && m_projectSession->HasActiveProject() && m_scene != nullptr
            && m_scene->GetRenderer().IsGpuResourcesReady())
        {
            ++s2p5ReadyFrame;
            SceneRenderer& sceneRenderer = m_scene->GetRenderer();
            ScreenSpaceEffects& effects = sceneRenderer.GetScreenSpaceEffects();
            DxrSettings& dxr = sceneRenderer.GetDxrSettings();

            if (s2p5ReadyFrame == 1)
            {
                dxr.SetEnabled(true);
                dxr.SetRenderingMode(RenderingMode::PathTraced);
                dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
                sceneRenderer.SetRenderDebugMode(RenderDebugMode::None);
                effects.SetRayReconstruction(false);
                effects.SetAntiAliasingMode(AntiAliasingMode::None);
                effects.SetExposure(-2.0f);
                glfwSetWindowSize(m_window, 1800, 1000);
                EngineLog::Info("benchmark", "S2-P5 settling direct path at window=1800x1000");
            }

            constexpr std::uint64_t kSettleFrames = 120;
            constexpr std::uint64_t kFramesPerCell = 40;
            constexpr std::uint64_t kModeCount = 11;
            constexpr std::uint64_t kExposureCount = 3;
            constexpr std::uint64_t kExtentCount = 2;
            constexpr std::uint64_t kCellCount = kModeCount * kExposureCount * kExtentCount;
            if (s2p5ReadyFrame > kSettleFrames)
            {
                ++s2p5MatrixFrame;

                // Small continuous translation exercises real temporal motion without reaching
                // the camera-cut threshold or obscuring the fine scene geometry.
                if (m_camera != nullptr)
                {
                    m_camera->SetPosition(
                        m_camera->GetPosition() + glm::vec3(0.00025f, 0.0f, 0.0f));
                }

                if ((s2p5MatrixFrame - 1) % kFramesPerCell == 0)
                {
                    const std::uint64_t cellIndex = (s2p5MatrixFrame - 1) / kFramesPerCell;
                    if (cellIndex < kCellCount)
                    {
                        const std::uint64_t cellsPerExtent = kModeCount * kExposureCount;
                        const std::uint64_t extentCase = cellIndex / cellsPerExtent;
                        const std::uint64_t localCell = cellIndex % cellsPerExtent;
                        const std::uint64_t modeIndex = localCell / kExposureCount;
                        const std::uint64_t exposureIndex = localCell % kExposureCount;
                        const float exposureEv = exposureIndex == 0 ? -2.0f
                            : exposureIndex == 1 ? 0.0f
                                                 : 2.0f;
                        if (cellIndex == cellsPerExtent)
                        {
                            glfwSetWindowSize(m_window, 1600, 1100);
                            EngineLog::Info(
                                "benchmark", "S2-P5 resize selected window=1600x1100");
                        }

                        const bool useRr = modeIndex >= 6;
                        const bool useDlss = modeIndex >= 1 && modeIndex <= 5;
                        const std::uint64_t qualityIndex = useRr ? modeIndex - 6
                            : useDlss ? modeIndex - 1
                                      : 0;
                        const char* modeName = "direct";
                        if (!useDlss && !useRr)
                        {
                            effects.SetRayReconstruction(false);
                            effects.SetAntiAliasingMode(AntiAliasingMode::None);
                        }
                        else
                        {
                            const AntiAliasingMode aa = qualityIndex == 0
                                ? AntiAliasingMode::DLAA
                                : AntiAliasingMode::DLSS;
                            const DlssPreset preset = qualityIndex <= 1 ? DlssPreset::Quality
                                : qualityIndex == 2 ? DlssPreset::Balanced
                                : qualityIndex == 3 ? DlssPreset::Performance
                                                    : DlssPreset::UltraPerformance;
                            effects.SetRayReconstruction(useRr);
                            effects.SetAntiAliasingMode(aa);
                            if (aa == AntiAliasingMode::DLSS)
                            {
                                effects.SetDlssPreset(preset);
                            }
                            constexpr const char* kQualityNames[] = {
                                "dlaa", "quality", "balanced", "performance", "ultra-performance"};
                            static std::string selectedMode;
                            selectedMode = std::string(useRr ? "rr-" : "dlss-")
                                + kQualityNames[qualityIndex];
                            modeName = selectedMode.c_str();
                        }
                        effects.SetExposure(exposureEv);

                        std::ostringstream marker;
                        marker << "S2-P5 cell selected index=" << cellIndex
                               << " extent-case=" << extentCase
                               << " mode=" << modeName
                               << " ev=" << exposureEv;
                        EngineLog::Info("benchmark", marker.str());
                    }
                }

                if (s2p5MatrixFrame > kCellCount * kFramesPerCell + 60)
                {
                    RequestForcedClose();
                }
            }
        }

        const bool automatedSceneReady = m_projectSession->HasActiveProject()
            && !m_projectChooser->IsBlockingEditor()
            && m_scene->GetRenderer().IsGpuResourcesReady()
            && !m_projectChooser->IsPresentingProjectLoad()
            && (!autoReopenOnce || autoReopenState == 3);
        if (m_automatedOpticalOrbitCapture != nullptr)
        {
            m_automatedOpticalOrbitCapture->PrepareFrame(
                automatedSceneReady, *m_scene, *m_camera);
        }

        try
        {
            const auto updateStart = std::chrono::steady_clock::now();
            ApplicationDetail::RunPhase("Update", [&]() { Update(deltaTime, frameDiagnostics); });
            frameDiagnostics.updateCpuMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - updateStart).count();
            if (ApplicationDetail::ConsumeConsoleCloseRequest())
            {
                RequestForcedClose();
                break;
            }
            const auto renderStart = std::chrono::steady_clock::now();
            ApplicationDetail::RunPhase("Render", [&]() { Render(); });
            frameDiagnostics.renderCpuMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
            frameDiagnostics.frameCpuMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameWorkStart).count();
            m_performancePanel->SetApplicationFrameDiagnostics(frameDiagnostics);
            if (m_automatedBenchmarkCapture != nullptr)
            {
                if (m_automatedBenchmarkCapture->ObserveFrame(
                        automatedSceneReady,
                        GfxContext::Get().GetGpuTimings(),
                        frameDiagnostics,
                        m_projectSession->GetProjectFilePath(),
                        m_projectSession->GetProjectRootDirectory(),
                        *m_camera,
                        m_scene->GetRenderer().GetDxrSettings(),
                        m_scene->GetRenderer().GetScreenSpaceEffects(),
                        static_cast<int>(m_scene->GetRenderer().GetRenderDebugMode())))
                {
                    RequestForcedClose();
                }
            }
            if (m_automatedOpticalOrbitCapture != nullptr
                && m_automatedOpticalOrbitCapture->ObserveFrame())
            {
                RequestForcedClose();
            }
            suppressedRepeatedFrameErrors = 0;
        }
        catch (const std::exception& exception)
        {
            const std::string described = DescribeException(exception);
            const std::string message = "Application frame: " + described;
            InputDiagnostics::Log(message.c_str());
            if (described.find("device removed") != std::string::npos
                || described.find("Present failed") != std::string::npos
                || described.find("HRESULT=0x887a") != std::string::npos)
            {
                EngineLog::Error("application", "Fatal GPU error â€” closing application.");
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            }
            if (described != lastLoggedFrameError)
            {
                lastLoggedFrameError = described;
                suppressedRepeatedFrameErrors = 0;
                std::cerr << message << "\n";
                if (m_projectSession != nullptr)
                {
                    m_projectSession->SetStatusMessage(message);
                }
            }
            else if (suppressedRepeatedFrameErrors < 3)
            {
                ++suppressedRepeatedFrameErrors;
                std::cerr << message << "\n";
            }
            else if (suppressedRepeatedFrameErrors == 3)
            {
                ++suppressedRepeatedFrameErrors;
                std::cerr << "Application frame: (suppressing repeated errors)\n";
                if (m_projectSession != nullptr)
                {
                    m_projectSession->SetStatusMessage("Application frame: " + described);
                }
            }
            RecoverInterruptedFrame();
        }
        catch (...)
        {
            EngineLog::Error("application", "Application frame: unknown exception");
            std::cerr << "Application frame: unknown exception\n";
            if (m_projectSession != nullptr)
            {
                m_projectSession->SetStatusMessage("Application frame: unknown exception");
            }
            RecoverInterruptedFrame();
        }
    }
}

void Application::HandleFatalGpuDeviceLoss(const std::string& reason)
{
    if (m_fatalGpuLossHandled)
    {
        return;
    }

    m_fatalGpuLossHandled = true;
    EngineLog::Error("application", reason);
    std::cerr << reason << "\n";

    NativeProgressWindow::Instance().End();
    if (m_projectChooser != nullptr)
    {
        m_projectChooser->ClearProjectLoadPresentation();
    }

    if (m_projectSession != nullptr)
    {
        if (m_projectSession->HasActiveProject())
        {
            m_projectSession->CloseProject();
        }

        m_projectSession->SetStatusMessage(reason);
    }

    RecoverInterruptedFrame();
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

void Application::RecoverInterruptedFrame()
{
    if (m_gfxFrameActive)
    {
        try
        {
            m_renderer->CancelFrame();
        }
        catch (...)
        {
        }

        m_gfxFrameActive = false;
    }

    if (m_imguiFrameActive)
    {
        try
        {
            m_imguiLayer->CancelInterruptedFrame();
        }
        catch (...)
        {
        }

        m_imguiFrameActive = false;
    }
}


