#include "engine/gizmos/CameraGizmoRenderer.h"
#include "engine/gizmos/ColliderGizmoRenderer.h"
#include "engine/gizmos/LightGizmoRenderer.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/passes/GridRenderer.h"

#include "app/scene/rendering/SceneRenderer.h"

#include "app/scene/rendering/GpuSceneBuilder.h"

#include "app/project/SceneProjectIODetail.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/system/BackgroundWork.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/platform/tooling/NativeProgressWindow.h"
#include "engine/platform/tooling/ProjectLoadBenchmark.h"
#include "engine/platform/tooling/ProjectLoadProgress.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"

#include "app/scene/editing/SceneEditor.h"
#include "engine/camera/Camera.h"
#include "engine/rhi/GfxContext.h"
#include "engine/components/LightComponent.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/rendering/passes/MeshShaderGBufferRenderer.h"
#include "engine/rendering/passes/MeshShaderShadowRenderer.h"
#include "engine/rendering/core/MotionVectorFrameState.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rendering/core/DxrSettings.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rendering/shaders/ShaderCache.h"
#include "engine/rendering/core/RenderingPipelineCache.h"
#include "engine/raytracing/pipeline/DxrShaderCache.h"
#include "engine/raytracing/core/DxrTrace.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
void SceneRenderer::WarmUpDxrPipelineIfNeeded()
{
    if (!m_dxrSettings.IsEnabled() || !GfxContext::Get().IsRaytracingSupported())
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        DxrBreadcrumb("render: WarmUpDxrPipeline skipped (frame recording)");
        return;
    }

    try
    {
        const RenderDebugMode debugMode = m_screenSpaceEffects != nullptr
            ? m_screenSpaceEffects->GetDebugMode()
            : RenderDebugMode::None;
        const bool pathTracingActive = m_dxrSettings.IsPathTracingActive();
        const bool restirDiTemporalEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirDiTemporalEnabled()
            && m_dxrSettings.GetRestirDiCandidateCount() > 0;
        const bool restirGiTemporalEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirGiInitialEnabled()
            && m_dxrSettings.IsRestirGiTemporalEnabled();
        const bool restirGiSpatialEnabled = pathTracingActive
            && !m_dxrSettings.IsPtReferenceConvergence()
            && m_dxrSettings.IsRestirGiInitialEnabled()
            && m_dxrSettings.IsRestirGiSpatialEnabled();

        // Keep startup proportional to the project's active render path. Debug views remain
        // eligible so a saved debug mode is ready for the first frame.
        const bool warmSmoke = debugMode == RenderDebugMode::RtDispatchSmoke;
        const bool warmPrimary = m_dxrSettings.IsDebugTraceEnabled() || IsRtPrimaryDebugMode(debugMode);
        const bool warmReflections = !pathTracingActive
            && (m_dxrSettings.IsReflectionsEnabled() || IsRtReflectionDebugMode(debugMode));
        const bool warmShadows = !pathTracingActive
            && (m_dxrSettings.IsShadowsEnabled() || IsRtShadowDebugMode(debugMode));
        const bool warmGi = !pathTracingActive
            && (m_dxrSettings.IsGiEnabled() || IsRtGiDebugMode(debugMode));
        const bool warmPathTracer = pathTracingActive;
        const bool warmRestir = restirDiTemporalEnabled || restirGiTemporalEnabled
            || restirGiSpatialEnabled;

        const int pendingPipelineCount =
            (warmSmoke && (m_dxrSmokeDispatch == nullptr || !m_dxrSmokeDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmPrimary && (m_dxrPrimaryDebugDispatch == nullptr || !m_dxrPrimaryDebugDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmReflections && (m_dxrReflectionsDispatch == nullptr || !m_dxrReflectionsDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmShadows && (m_dxrShadowsDispatch == nullptr || !m_dxrShadowsDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmGi && (m_dxrGiDispatch == nullptr || !m_dxrGiDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmPathTracer && (m_dxrPathTracerDispatch == nullptr || !m_dxrPathTracerDispatch->IsPipelineReady()) ? 1 : 0)
            + (warmRestir && (m_dxrRestirDispatch == nullptr || !m_dxrRestirDispatch->IsPipelineReady()) ? 1 : 0);
        if (pendingPipelineCount == 0)
        {
            return;
        }

        // Shader-library compilation is CPU work and does not touch the D3D12 device. Compile
        // every exact library permutation needed by this project before creating the dependent
        // RTPSOs in their normal (device-safe) order below. Keep the worker count bounded: a
        // project open must leave CPU time for the desktop, compositor, input, and UI threads.
        struct ShaderPrewarmRequest
        {
            const char* libraryPath = nullptr;
            DxrShaderLibraryCompileOptions options;
        };
        std::vector<ShaderPrewarmRequest> shaderPrewarmRequests;
        const auto queueShaderPrewarm = [&](const char* libraryPath, DxrShaderLibraryCompileOptions options) {
            shaderPrewarmRequests.push_back({libraryPath, std::move(options)});
        };
        const auto queueDefaultShaderPrewarm = [&](const char* libraryPath) {
            queueShaderPrewarm(libraryPath, DxrShaderCache::MakeActiveDeviceCompileOptions());
        };

        if (warmSmoke)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrSmokeLibraryShader);
        }
        if (warmPrimary)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrPrimaryDebugLibraryShader);
        }
        if (warmReflections)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrReflectionsLibraryShader);
        }
        if (warmShadows)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrShadowsLibraryShader);
        }
        if (warmGi)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrGiLibraryShader);
        }
        if (warmPathTracer)
        {
            queueShaderPrewarm(
                EngineConstants::DxrPathTracerLibraryShader,
                DxrShaderCache::MakeActiveDeviceCompileOptions(false));
            queueShaderPrewarm(
                EngineConstants::DxrPathTracerLibraryShader,
                DxrShaderCache::MakeActiveDeviceCompileOptions(true));
            if (GfxContext::Get().IsShaderExecutionReorderingSupported())
            {
                DxrShaderLibraryCompileOptions serOptions =
                    DxrShaderCache::MakeActiveDeviceCompileOptions(false);
                serOptions.serPermutation = true;
                serOptions.targetProfile = "lib_6_9";
                queueShaderPrewarm(EngineConstants::DxrPathTracerLibraryShader, serOptions);

                serOptions = DxrShaderCache::MakeActiveDeviceCompileOptions(true);
                serOptions.serPermutation = true;
                serOptions.targetProfile = "lib_6_9";
                queueShaderPrewarm(EngineConstants::DxrPathTracerLibraryShader, serOptions);
            }
        }
        if (warmRestir)
        {
            queueDefaultShaderPrewarm(EngineConstants::DxrRestirLibraryShader);
        }

        // PBR would otherwise compile for the first time while recording geometry after DXR is
        // already ready. Its bytecode/PSO construction is independent of DXR shader compilation,
        // so overlap it with that existing CPU warm-up and retain the result just long enough for
        // Material::EnsureShader() to take the cache reference in the first geometry pass.
        const bool needsPbrPrewarm = m_preWarmedPbrShader == nullptr;
        const std::size_t compilationWorkerBudget = BackgroundWork::ResponsiveWorkerCount(
            shaderPrewarmRequests.size() + (needsPbrPrewarm ? 1u : 0u),
            std::thread::hardware_concurrency());

        // Reserve one bounded worker for PBR while DXR libraries compile. On a one-worker system,
        // finish DXR first and compile PBR on the caller instead of exceeding the responsiveness
        // budget merely to overlap the two tasks.
        std::future<std::shared_ptr<Shader>> pbrPrewarmJob;
        const bool runPbrConcurrently = needsPbrPrewarm && compilationWorkerBudget > 1;
        if (runPbrConcurrently)
        {
            pbrPrewarmJob = std::async(std::launch::async, []() {
                BackgroundWork::LowerCurrentThreadPriority();
                return ShaderCache::Load(EngineConstants::LitVertexShader, EngineConstants::PbrFragmentShader);
            });
        }

        ProjectLoadBenchmark::ScopedPhase shaderPrewarmPhase("renderer.dxr_shader_library_prewarm");
        const int shaderLibraryJobCount = static_cast<int>(shaderPrewarmRequests.size());
        int completedShaderLibraryJobCount = 0;
        ProjectLoadProgress::Report(
            "Compiling ray tracing shader libraries (0/" + std::to_string(shaderLibraryJobCount) + ")...",
            ProjectLoadProgress::kDxrWarmupStart);

        const std::size_t dxrWorkerCount = std::max<std::size_t>(
            1,
            compilationWorkerBudget - (runPbrConcurrently ? 1u : 0u));
        std::atomic<std::size_t> nextShaderRequest{0};
        std::atomic<std::size_t> completedShaderRequests{0};
        std::atomic<bool> stopShaderWorkers{false};
        std::mutex shaderFailureMutex;
        std::exception_ptr shaderFailure;
        const auto compileShaderWorker = [&]() {
            BackgroundWork::LowerCurrentThreadPriority();
            while (!stopShaderWorkers.load(std::memory_order_acquire))
            {
                const std::size_t requestIndex = nextShaderRequest.fetch_add(1, std::memory_order_relaxed);
                if (requestIndex >= shaderPrewarmRequests.size())
                {
                    return;
                }

                try
                {
                    const ShaderPrewarmRequest& request = shaderPrewarmRequests[requestIndex];
                    (void)DxrShaderCache::Load(request.libraryPath, request.options);
                    completedShaderRequests.fetch_add(1, std::memory_order_release);
                }
                catch (...)
                {
                    {
                        std::lock_guard<std::mutex> lock(shaderFailureMutex);
                        if (shaderFailure == nullptr)
                        {
                            shaderFailure = std::current_exception();
                        }
                    }
                    stopShaderWorkers.store(true, std::memory_order_release);
                    return;
                }
            }
        };
        std::vector<std::future<void>> shaderPrewarmWorkers;
        shaderPrewarmWorkers.reserve(dxrWorkerCount);
        for (std::size_t workerIndex = 0; workerIndex < dxrWorkerCount; ++workerIndex)
        {
            shaderPrewarmWorkers.emplace_back(std::async(std::launch::async, compileShaderWorker));
        }
        while (completedShaderLibraryJobCount < shaderLibraryJobCount)
        {
            const int completedRequests = static_cast<int>(completedShaderRequests.load(std::memory_order_acquire));
            while (completedShaderLibraryJobCount < completedRequests)
            {
                ++completedShaderLibraryJobCount;
                ProjectLoadProgress::Report(
                    "Compiling ray tracing shader libraries ("
                        + std::to_string(completedShaderLibraryJobCount)
                        + "/" + std::to_string(shaderLibraryJobCount) + ")...",
                    ProjectLoadProgress::DxrShaderLibraryWarmup(
                        static_cast<float>(completedShaderLibraryJobCount)
                        / static_cast<float>(shaderLibraryJobCount)));
            }

            if (stopShaderWorkers.load(std::memory_order_acquire))
            {
                break;
            }

            // Compilation runs on worker threads. Yield briefly rather than spin while still
            // allowing the independently-owned native progress window to present completions.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        for (std::future<void>& worker : shaderPrewarmWorkers)
        {
            worker.get();
        }
        if (shaderFailure != nullptr)
        {
            std::rethrow_exception(shaderFailure);
        }

        if (needsPbrPrewarm)
        {
            ProjectLoadProgress::Report(
                "Preparing the first scene shader...",
                ProjectLoadProgress::kDxrShaderLibraryWarmupEnd);
            if (pbrPrewarmJob.valid())
            {
                m_preWarmedPbrShader = pbrPrewarmJob.get();
            }
            else
            {
                m_preWarmedPbrShader = ShaderCache::Load(
                    EngineConstants::LitVertexShader,
                    EngineConstants::PbrFragmentShader);
            }
        }

        int warmedPipelineCount = 0;
        const auto reportPipelineBegin = [&](const char* message) {
            const float progress = ProjectLoadProgress::DxrPipelineWarmup(
                static_cast<float>(warmedPipelineCount) / static_cast<float>(pendingPipelineCount));
            ProjectLoadProgress::Report(message, progress);
        };
        const auto markPipelineComplete = [&]() {
            ++warmedPipelineCount;
            const float progress = ProjectLoadProgress::DxrPipelineWarmup(
                static_cast<float>(warmedPipelineCount) / static_cast<float>(pendingPipelineCount));
            ProjectLoadProgress::SetProgress(progress);
        };
        const auto warmPipeline = [](const char* benchmarkPhase, auto&& callback) {
            ProjectLoadBenchmark::ScopedPhase phase(benchmarkPhase);
            callback();
        };

        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded begin");
        if (warmSmoke && (m_dxrSmokeDispatch == nullptr || !m_dxrSmokeDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR smoke pipeline...");
            if (m_dxrSmokeDispatch == nullptr)
            {
                m_dxrSmokeDispatch = std::make_unique<DxrSmokeDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.smoke", [&]() { m_dxrSmokeDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmPrimary && (m_dxrPrimaryDebugDispatch == nullptr || !m_dxrPrimaryDebugDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR primary-debug pipeline...");
            if (m_dxrPrimaryDebugDispatch == nullptr)
            {
                m_dxrPrimaryDebugDispatch = std::make_unique<DxrPrimaryDebugDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.primary_debug",
                [&]() { m_dxrPrimaryDebugDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmReflections && (m_dxrReflectionsDispatch == nullptr || !m_dxrReflectionsDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR reflections pipeline...");
            if (m_dxrReflectionsDispatch == nullptr)
            {
                m_dxrReflectionsDispatch = std::make_unique<DxrReflectionsDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.reflections",
                [&]() { m_dxrReflectionsDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmShadows && (m_dxrShadowsDispatch == nullptr || !m_dxrShadowsDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR shadows pipeline...");
            if (m_dxrShadowsDispatch == nullptr)
            {
                m_dxrShadowsDispatch = std::make_unique<DxrShadowsDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.shadows",
                [&]() { m_dxrShadowsDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmGi && (m_dxrGiDispatch == nullptr || !m_dxrGiDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling DXR GI pipeline...");
            if (m_dxrGiDispatch == nullptr)
            {
                m_dxrGiDispatch = std::make_unique<DxrGiDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.gi", [&]() { m_dxrGiDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        if (warmPathTracer && (m_dxrPathTracerDispatch == nullptr || !m_dxrPathTracerDispatch->IsPipelineReady()))
        {
            if (m_dxrPathTracerDispatch == nullptr)
            {
                m_dxrPathTracerDispatch = std::make_unique<DxrPathTracerDispatch>();
            }
            warmPipeline(
                "renderer.dxr_warmup.path_tracer",
                [&]() {
                    m_dxrPathTracerDispatch->WarmUpPipelineIfNeeded(
                        [&](const int step, const int stepCount, const char* label) {
                            const float completedVariantFraction =
                                static_cast<float>(step - 1) / static_cast<float>(stepCount);
                            const float pipelineFraction =
                                (static_cast<float>(warmedPipelineCount) + completedVariantFraction)
                                / static_cast<float>(pendingPipelineCount);
                            ProjectLoadProgress::Report(
                                "Compiling path tracer pipeline (" + std::to_string(step) + "/"
                                    + std::to_string(stepCount) + "): " + label + "...",
                                ProjectLoadProgress::DxrPipelineWarmup(pipelineFraction));
                        });
                });
            markPipelineComplete();
        }
        if (warmRestir && (m_dxrRestirDispatch == nullptr || !m_dxrRestirDispatch->IsPipelineReady()))
        {
            reportPipelineBegin("Compiling ReSTIR pipeline...");
            if (m_dxrRestirDispatch == nullptr)
            {
                m_dxrRestirDispatch = std::make_unique<DxrRestirDispatch>();
            }
            warmPipeline("renderer.dxr_warmup.restir", [&]() { m_dxrRestirDispatch->WarmUpPipelineIfNeeded(); });
            markPipelineComplete();
        }
        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded end");
    }
    catch (const std::exception& exception)
    {
        EngineLog::Error(
            "dxr",
            std::string("WarmUpDxrPipelineIfNeeded failed: ") + SafeExceptionMessage(exception));
        DxrBreadcrumb("render: WarmUpDxrPipelineIfNeeded failed");
    }
}

