#include "app/core/benchmark/Capture.h"

#include "app/scene/document/Scene.h"
#include "app/scene/rendering/SceneRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/CaptureManifest.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/rendering/core/DxrSettings.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rhi/GfxContext.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    int ReadPositiveEnvironmentInt(const char* name, const int defaultValue)
    {
        const char* raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0')
        {
            return defaultValue;
        }

        try
        {
            const int value = std::stoi(raw);
            if (value <= 0)
            {
                throw std::runtime_error("must be positive");
            }
            return value;
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(std::string(name) + " must be a positive integer.");
        }
    }

    int ReadNonNegativeEnvironmentInt(const char* name, const int defaultValue)
    {
        const char* raw = std::getenv(name);
        if (raw == nullptr || raw[0] == '\0')
        {
            return defaultValue;
        }

        try
        {
            const int value = std::stoi(raw);
            if (value < 0)
            {
                throw std::runtime_error("must be non-negative");
            }
            return value;
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(std::string(name) + " must be a non-negative integer.");
        }
    }

    float FindGpuTiming(const std::vector<GpuProfiler::Entry>& timings, const char* name)
    {
        const auto found = std::find_if(
            timings.begin(),
            timings.end(),
            [name](const GpuProfiler::Entry& entry) { return entry.name == name; });
        return found != timings.end() ? found->milliseconds : -1.0f;
    }

    std::string ReadOptionalEnvironmentString(const char* name)
    {
        const char* raw = std::getenv(name);
        return raw != nullptr && raw[0] != '\0' ? raw : std::string{};
    }

    std::string Fnv1a64(const std::vector<std::uint8_t>& bytes)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::uint8_t value : bytes)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        std::ostringstream result;
        result << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
        return result.str();
    }

    std::string Fnv1a64(const std::string& bytes)
    {
        return Fnv1a64(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    }

    std::string HashFileContents(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("Could not read capture asset: " + path.string());
        }
        const std::vector<std::uint8_t> bytes{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        return Fnv1a64(bytes);
    }

    std::string HashAssetManifest(const std::filesystem::path& projectRoot)
    {
        if (!std::filesystem::is_directory(projectRoot))
        {
            throw std::runtime_error("Capture project root is unavailable: " + projectRoot.string());
        }

        std::vector<std::filesystem::path> assets;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(projectRoot))
        {
            const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), projectRoot);
            // Editor layout is session state, not a scene asset. Including it would make a capture
            // manifest depend on unrelated docking changes and defeat reproducible asset identity.
            if (entry.is_regular_file() && (relativePath.empty() || *relativePath.begin() != ".editor"))
            {
                assets.push_back(relativePath);
            }
        }
        std::sort(assets.begin(), assets.end());

        std::string normalizedManifest;
        for (const std::filesystem::path& relativePath : assets)
        {
            const std::filesystem::path absolutePath = projectRoot / relativePath;
            normalizedManifest += relativePath.generic_string();
            normalizedManifest += '\n';
            normalizedManifest += HashFileContents(absolutePath);
            normalizedManifest += '\n';
        }
        return Fnv1a64(normalizedManifest);
    }

    nlohmann::json MakeCapabilityRecord(const DxrRuntimeSnapshot& snapshot)
    {
        return nlohmann::json::parse(SerializeDxrRuntimeSnapshotJson(snapshot));
    }

    nlohmann::json BuildSemanticConfiguration(
        const std::string& revision,
        const std::string& projectFilePath,
        const std::string& projectRootDirectory,
        const Camera& camera,
        const DxrSettings& dxrSettings,
        const ScreenSpaceEffects& screenSpaceEffects,
        const int renderDebugMode,
        const int warmupSeconds,
        const int warmupFrames,
        const int sampleFrames)
    {
        if (revision.empty())
        {
            throw std::runtime_error("S0-P5 capture requires GAME_ENGINE_CAPTURE_REVISION.");
        }
        if (projectFilePath.empty() || projectRootDirectory.empty())
        {
            throw std::runtime_error("S0-P5 capture requires an active project with a resolved root directory.");
        }

        const glm::vec3 cameraPosition = camera.GetPosition();
        const GfxContext& gfx = GfxContext::Get();
        const DxrRuntimeSnapshot& runtimeSnapshot = gfx.GetDxrRuntimeSnapshot();
        return {
            {"revision", revision},
            {"scene", {{"project_path", std::filesystem::path(projectFilePath).generic_string()},
                         {"project_content_hash", HashFileContents(projectFilePath)},
                         {"asset_manifest_content_hash", HashAssetManifest(projectRootDirectory)}}},
            {"camera", {{"owner", "editor_camera"},
                          {"pose_or_path", {{"position", {cameraPosition.x, cameraPosition.y, cameraPosition.z}},
                                            {"yaw_degrees", camera.GetYaw()},
                                            {"pitch_degrees", camera.GetPitch()},
                                            {"fov_degrees", camera.GetFov()},
                                            {"near_plane", camera.GetNearPlane()},
                                            {"far_plane", camera.GetFarPlane()},
                                            {"jitter_policy", "renderer_default_sequence"}}}}},
            {"viewport", {{"id", 0},
                            {"output_extent", {gfx.GetWidth(), gfx.GetHeight()}},
                            {"render_extent", {screenSpaceEffects.GetRenderWidth(), screenSpaceEffects.GetRenderHeight()}}}},
            {"path_tracer", {{"mode", DxrSettings::RenderingModeToString(dxrSettings.GetRenderingMode())},
                                {"spp", 1},
                                {"max_bounces", dxrSettings.GetPtMaxBounces()},
                                {"seed", {{"algorithm", "pcg3d(pixel, path_tracer_frame_index, sample_index)"},
                                          {"frame_index_owner", "DxrPathTracerDispatch"}}},
                                {"russian_roulette", dxrSettings.IsPtRussianRouletteEnabled()},
                                {"firefly_clamp", dxrSettings.IsPtFireflyClampEnabled()},
                                {"deterministic_optical_split",
                                 dxrSettings.IsPtDeterministicOpticalSplitEnabled()},
                                {"optical_motion_replay",
                                 dxrSettings.IsPtOpticalMotionReplayEnabled()},
                                {"mirror_chain_psr", dxrSettings.IsPtMirrorChainPsrEnabled()},
                                {"psr_max_bounces", dxrSettings.GetPtPsrMaxBounces()},
                                {"psr_subpixel_threshold", dxrSettings.GetPtPsrSubpixelThreshold()}}},
            {"restir", {{"di_candidates", dxrSettings.GetRestirDiCandidateCount()},
                          {"di_temporal", dxrSettings.IsRestirDiTemporalEnabled()},
                          {"gi_initial", dxrSettings.IsRestirGiInitialEnabled()},
                          {"gi_temporal", dxrSettings.IsRestirGiTemporalEnabled()},
                          {"gi_spatial", dxrSettings.IsRestirGiSpatialEnabled()},
                          {"diagnostic_mode", static_cast<int>(dxrSettings.GetRestirGiSpatialDiagnosticMode())}}},
            {"reconstruction", {{"feature", screenSpaceEffects.GetRayReconstruction() ? "ray_reconstruction" : "none"},
                                  {"anti_aliasing_mode",
                                   static_cast<int>(screenSpaceEffects.GetAntiAliasingMode())},
                                  {"quality", static_cast<int>(screenSpaceEffects.GetDlssPreset())},
                                  {"rr_preset", static_cast<int>(screenSpaceEffects.GetRrPreset())},
                                  {"independent_optical_rr_layers",
                                   dxrSettings.IsPtIndependentOpticalRrLayersEnabled()},
                                  {"rr_bundle_mode", dxrSettings.GetPtRrBundleMode()}}},
            {"ser", {{"requested_policy", runtimeSnapshot.requestedSerPolicy},
                       {"selected_permutation", runtimeSnapshot.selectedPermutation},
                       {"dispatched_permutation", runtimeSnapshot.dispatchedPermutation},
                       {"fallback_reason", runtimeSnapshot.fallbackReason}}},
            {"diagnostics", {{"frame_trace", std::getenv("GAME_ENGINE_FRAME_DEBUG") != nullptr},
                               {"gpu_events", true},
                               {"s0p2_token_trace", std::getenv("GAME_ENGINE_S0P2_TRACE") != nullptr},
                               {"s0p3_history_trace", std::getenv("GAME_ENGINE_S0P3_TRACE") != nullptr},
                               {"s0p4_scopes", true}}},
            {"window", {{"warmup_seconds", warmupSeconds},
                          {"warmup_frames", warmupFrames},
                          {"sample_frames", sampleFrames},
                          {"capture_frame_index", "next_normal_frame_after_timing_window"}}},
            {"s0_capability", MakeCapabilityRecord(runtimeSnapshot)},
        };
    }
}

AutomatedBenchmarkCapture::AutomatedBenchmarkCapture(
    std::string outputPath,
    std::string imageOutputPath,
    std::string manifestOutputPath,
    std::string manifestInputPath,
    std::string revision,
    std::string comparisonMode,
    const int warmupSeconds,
    const int warmupFrames,
    const int sampleFrames)
    : m_outputPath(std::move(outputPath))
    , m_imageOutputPath(std::move(imageOutputPath))
    , m_manifestOutputPath(std::move(manifestOutputPath))
    , m_manifestInputPath(std::move(manifestInputPath))
    , m_revision(std::move(revision))
    , m_comparisonMode(std::move(comparisonMode))
    , m_warmupSeconds(warmupSeconds)
    , m_warmupFrames(warmupFrames)
    , m_sampleFrames(sampleFrames)
{
}

std::unique_ptr<AutomatedBenchmarkCapture> AutomatedBenchmarkCapture::CreateFromEnvironment()
{
    const char* rawOutput = std::getenv("GAME_ENGINE_BENCHMARK_OUTPUT");
    if (rawOutput == nullptr || rawOutput[0] == '\0')
    {
        return nullptr;
    }

    const int warmupSeconds = ReadNonNegativeEnvironmentInt("GAME_ENGINE_BENCHMARK_WARMUP_SECONDS", 10);
    const int warmupFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_BENCHMARK_WARMUP_FRAMES", 120);
    const int sampleFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES", 300);
    const std::string imageOutputPath = ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_IMAGE_OUTPUT");
    const std::string manifestOutputPath = ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT");
    const std::string manifestInputPath = ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_MANIFEST_INPUT");
    const std::string revision = ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_REVISION");
    const bool s0p5Capture = std::getenv("GAME_ENGINE_S0P5_CAPTURE") != nullptr;
    if (s0p5Capture && (imageOutputPath.empty() || manifestOutputPath.empty() || revision.empty()))
    {
        throw std::runtime_error(
            "GAME_ENGINE_S0P5_CAPTURE requires GAME_ENGINE_CAPTURE_IMAGE_OUTPUT, "
            "GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT, and GAME_ENGINE_CAPTURE_REVISION.");
    }
    std::string comparisonMode = ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_COMPARISON_MODE");
    if (comparisonMode.empty())
    {
        comparisonMode = "statistical";
    }
    if (comparisonMode != "exact" && comparisonMode != "statistical")
    {
        throw std::runtime_error("GAME_ENGINE_CAPTURE_COMPARISON_MODE must be exact or statistical.");
    }
    return std::unique_ptr<AutomatedBenchmarkCapture>(
        new AutomatedBenchmarkCapture(
            rawOutput,
            imageOutputPath,
            manifestOutputPath,
            manifestInputPath,
            revision,
            comparisonMode,
            warmupSeconds,
            warmupFrames,
            sampleFrames));
}

bool AutomatedBenchmarkCapture::ObserveFrame(
    const bool sceneReady,
    const std::vector<GpuProfiler::Entry>& gpuTimings,
    const ApplicationFrameDiagnostics& applicationTimings,
    const std::string& projectFilePath,
    const std::string& projectRootDirectory,
    const Camera& camera,
    const DxrSettings& dxrSettings,
    const ScreenSpaceEffects& screenSpaceEffects,
    const int renderDebugMode)
{
    if (m_complete)
    {
        return true;
    }

    if (m_imageCaptureRequested)
    {
        GfxContext::PresentedImageCapture image;
        if (!GfxContext::Get().TryConsumePresentedImageCapture(image))
        {
            return false;
        }
        const float captureGpuMs = FindGpuTiming(gpuTimings, "S0-P5/Capture final output");

        const std::filesystem::path imagePath(m_imageOutputPath);
        std::error_code error;
        if (!imagePath.parent_path().empty())
        {
            std::filesystem::create_directories(imagePath.parent_path(), error);
        }
        if (error)
        {
            throw std::runtime_error("Could not create capture image directory: " + error.message());
        }
        std::ofstream imageOutput(m_imageOutputPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!imageOutput.is_open())
        {
            throw std::runtime_error("Could not open capture image output: " + m_imageOutputPath);
        }
        imageOutput.write(
            reinterpret_cast<const char*>(image.rgba8.data()),
            static_cast<std::streamsize>(image.rgba8.size()));
        if (!imageOutput)
        {
            throw std::runtime_error("Could not write capture image output: " + m_imageOutputPath);
        }

        const std::filesystem::path metadataPath = imagePath.string() + ".json";
        std::ofstream metadata(metadataPath, std::ios::out | std::ios::trunc);
        if (!metadata.is_open())
        {
            throw std::runtime_error("Could not open capture image metadata: " + metadataPath.string());
        }
        const double captureDrainCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - m_imageCaptureRequestTime).count();
        metadata << "{\"record_type\":\"s0p5_final_output\",\"format\":\"rgba8\",\"extent\":["
                 << image.width << ',' << image.height << "],\"comparison_mode\":\"" << m_comparisonMode
                 << "\",\"rgba8_fnv1a64\":\"" << Fnv1a64(image.rgba8)
                 << "\",\"capture_gpu_ms\":";
        if (captureGpuMs >= 0.0f)
        {
            metadata << std::fixed << std::setprecision(6) << captureGpuMs;
        }
        else
        {
            // Profiler entries become visible one completed frame later than the readback fence.
            // Do not invent a GPU duration; the separate timing window and measured CPU drain remain
            // explicit evidence for this capture-only operation.
            metadata << "null";
        }
        metadata << ",\"capture_gpu_timing_status\":\""
                 << (captureGpuMs >= 0.0f ? "available" : "deferred_profiler_result")
                 << "\",\"capture_drain_cpu_ms\":" << std::fixed << std::setprecision(6)
                 << captureDrainCpuMs << "}";
        m_complete = true;
        EngineLog::Info("benchmark", "S0-P5 final-output capture complete: " + metadataPath.string());
        return true;
    }

    if (!sceneReady || gpuTimings.empty())
    {
        return false;
    }

    // Timestamp results are read from a completed swapchain slice. Requiring this scope avoids
    // accidentally treating the first empty profiler result after project startup as a sample.
    if (FindGpuTiming(gpuTimings, "Frame GPU span") < 0.0f)
    {
        return false;
    }

    if (!m_manifestWritten && (!m_manifestOutputPath.empty() || !m_manifestInputPath.empty()))
    {
        nlohmann::json semantic = BuildSemanticConfiguration(
            m_revision,
            projectFilePath,
            projectRootDirectory,
            camera,
            dxrSettings,
            screenSpaceEffects,
            renderDebugMode,
            m_warmupSeconds,
            m_warmupFrames,
            m_sampleFrames);
        // renderDebugMode is intentionally captured as a diagnostic state, rather than used to
        // select any renderer behavior.
        semantic["diagnostics"]["render_debug_mode"] = renderDebugMode;

        if (!m_manifestInputPath.empty())
        {
            const nlohmann::json replay = CaptureManifest::LoadAndValidate(m_manifestInputPath);
            if (replay.at("semantic") != semantic)
            {
                throw std::runtime_error(
                    "Capture manifest replay does not exactly match current authoritative state; "
                    "refusing silent fallback or settings mutation.");
            }
        }

        if (!m_manifestOutputPath.empty())
        {
            const nlohmann::json manifest = CaptureManifest::Create(semantic, m_comparisonMode);
            const std::filesystem::path manifestPath(m_manifestOutputPath);
            std::error_code error;
            if (!manifestPath.parent_path().empty())
            {
                std::filesystem::create_directories(manifestPath.parent_path(), error);
            }
            if (error)
            {
                throw std::runtime_error("Could not create capture manifest directory: " + error.message());
            }
            std::ofstream output(manifestPath, std::ios::out | std::ios::trunc);
            if (!output.is_open())
            {
                throw std::runtime_error("Could not write capture manifest: " + manifestPath.string());
            }
            output << manifest.dump(2) << '\n';
            if (!output)
            {
                throw std::runtime_error("Could not finish capture manifest: " + manifestPath.string());
            }
            EngineLog::Info(
                "benchmark",
                "S0-P5 manifest captured: " + manifestPath.string() + " hash="
                    + manifest.at("semantic_hash").get<std::string>());
        }
        m_manifestWritten = true;
    }

    if (!m_started)
    {
        const std::filesystem::path outputPath(m_outputPath);
        std::error_code error;
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path(), error);
        }
        if (error)
        {
            throw std::runtime_error(
                "Could not create benchmark output directory: " + error.message());
        }

        m_output = std::make_unique<std::ofstream>(m_outputPath, std::ios::out | std::ios::trunc);
        if (!m_output->is_open())
        {
            throw std::runtime_error("Could not open benchmark output: " + m_outputPath);
        }
        *m_output
            << "sample_index,frame_gpu_span_ms,path_tracer_ms,primary_rays_ms,"
            << "restir_spatial_0_ms,restir_temporal_ms,dlss_evaluate_ms,cpu_frame_ms,cpu_render_ms\n";
        m_output->setf(std::ios::fixed);
        *m_output << std::setprecision(6);
        m_started = true;
        m_readyTime = std::chrono::steady_clock::now();
        EngineLog::Info(
            "benchmark",
            "Timestamp capture armed: warmup=" + std::to_string(m_warmupSeconds)
                + "s + " + std::to_string(m_warmupFrames) + " frames"
                + ", samples=" + std::to_string(m_sampleFrames));
    }

    if (m_warmupFrames > 0)
    {
        --m_warmupFrames;
        return false;
    }
    if (std::chrono::steady_clock::now() - m_readyTime < std::chrono::seconds(m_warmupSeconds))
    {
        return false;
    }

    *m_output
        << m_capturedFrames << ','
        << FindGpuTiming(gpuTimings, "Frame GPU span") << ','
        << FindGpuTiming(gpuTimings, "Path tracer") << ','
        << FindGpuTiming(gpuTimings, "Path tracer/Primary rays") << ','
        << FindGpuTiming(gpuTimings, "Path tracer/ReSTIR spatial 0") << ','
        << FindGpuTiming(gpuTimings, "Path tracer/ReSTIR temporal") << ','
        << FindGpuTiming(gpuTimings, "DLSS/Evaluate") << ','
        << applicationTimings.frameCpuMs << ','
        << applicationTimings.renderCpuMs << '\n';
    m_output->flush();

    ++m_capturedFrames;
    if (m_capturedFrames < m_sampleFrames)
    {
        return false;
    }

    m_output->close();
    if (!m_imageOutputPath.empty())
    {
        if (!GfxContext::Get().RequestPresentedImageCapture())
        {
            throw std::runtime_error("Could not queue S0-P5 final-output capture; refusing silent fallback.");
        }
        m_imageCaptureRequested = true;
        m_imageCaptureRequestTime = std::chrono::steady_clock::now();
        EngineLog::Info("benchmark", "S0-P5 final-output capture queued on the next normal frame.");
        return false;
    }
    m_complete = true;
    EngineLog::Info("benchmark", "Timestamp capture complete: " + m_outputPath);
    return true;
}

namespace
{
    struct OpticalCaptureMode
    {
        RenderDebugMode debugMode = RenderDebugMode::None;
        bool rayReconstruction = true;
        AntiAliasingMode antiAliasing = AntiAliasingMode::DLAA;
    };

    OpticalCaptureMode ResolveOpticalCaptureMode(const std::string& mode)
    {
        if (mode == "raw-radiance")
        {
            return {RenderDebugMode::None, false, AntiAliasingMode::None};
        }
        if (mode == "final-rr")
        {
            return {RenderDebugMode::None, true, AntiAliasingMode::DLAA};
        }

        const std::pair<const char*, RenderDebugMode> rrModes[] = {
            {"raw-reflection", RenderDebugMode::PtOpticalRawReflection},
            {"raw-transmission", RenderDebugMode::PtOpticalRawTransmission},
            {"reconstructed-reflection", RenderDebugMode::PtOpticalReconstructedReflection},
            {"reconstructed-transmission", RenderDebugMode::PtOpticalReconstructedTransmission},
            {"reflection-delta", RenderDebugMode::PtOpticalReflectionReconstructionDelta},
            {"transmission-delta", RenderDebugMode::PtOpticalTransmissionReconstructionDelta},
            {"reflection-guide-diffuse", RenderDebugMode::RrDiffuseAlbedo},
            {"reflection-guide-specular", RenderDebugMode::RrSpecularAlbedo},
            {"reflection-guide-normal", RenderDebugMode::RrNormalRoughness},
            {"transmission-guide-diffuse", RenderDebugMode::RrTransmissionDiffuseAlbedo},
            {"transmission-guide-specular", RenderDebugMode::RrTransmissionSpecularAlbedo},
            {"transmission-guide-normal", RenderDebugMode::RrTransmissionNormalRoughness},
        };
        for (const auto& [name, debugMode] : rrModes)
        {
            if (mode == name)
            {
                return {debugMode, true, AntiAliasingMode::DLAA};
            }
        }

        const std::pair<const char*, RenderDebugMode> rawAovModes[] = {
            {"coverage-fresnel", RenderDebugMode::PtOpticalCoverageFresnel},
            {"reflection-reprojection", RenderDebugMode::PtOpticalReflectionReprojection},
            {"transmission-reprojection", RenderDebugMode::PtOpticalTransmissionReprojection},
            {"reflection-replay-status", RenderDebugMode::PtOpticalReflectionReplayStatus},
            {"transmission-replay-status", RenderDebugMode::PtOpticalTransmissionReplayStatus},
            {"transmission-attribution", RenderDebugMode::PtOpticalTransmissionAttribution},
            {"transmission-environment", RenderDebugMode::PtOpticalTransmissionEnvironment},
            {"transmission-receiver", RenderDebugMode::PtOpticalTransmissionReceiver},
            {"transmission-deep-bounce", RenderDebugMode::PtOpticalTransmissionDeepBounce},
            {"mirror-chain-owner", RenderDebugMode::PtMirrorChainOwner},
            {"mirror-chain-length", RenderDebugMode::PtMirrorChainLength},
            {"mirror-chain-confidence", RenderDebugMode::PtMirrorChainConfidence},
            {"mirror-chain-receiver-id", RenderDebugMode::PtMirrorChainReceiverId},
            {"mirror-chain-receiver-depth", RenderDebugMode::PtMirrorChainReceiverDepth},
            {"mirror-chain-receiver-motion", RenderDebugMode::PtMirrorChainReceiverMotion},
            {"psr-terminal-reason", RenderDebugMode::PtPsrTerminalReason},
            {"psr-projected-span", RenderDebugMode::PtPsrProjectedSpan},
            {"psr-throughput", RenderDebugMode::PtPsrThroughput},
            {"psr-receiver-signal", RenderDebugMode::PtPsrReceiverSignal},
        };
        for (const auto& [name, debugMode] : rawAovModes)
        {
            if (mode == name)
            {
                return {debugMode, false, AntiAliasingMode::None};
            }
        }

        throw std::runtime_error("Unknown GAME_ENGINE_OPTICAL_CAPTURE_MODE: " + mode);
    }

    int FindNamedObject(const Scene& scene, const std::string& name)
    {
        const auto& objects = scene.GetObjects();
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            if (objects[index].GetName() == name)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
}

class AutomatedOpticalOrbitCapture::Impl
{
public:
    std::filesystem::path outputDirectory;
    std::string mode;
    std::string targetName;
    OpticalCaptureMode captureMode;
    int warmupFrames = 120;
    int orbitFrames = 180;
    int revolutions = 1;
    int frameStride = 12;
    bool configured = false;
    bool complete = false;
    int warmupRemaining = 0;
    int orbitFrame = 0;
    int captureIndex = 0;
    bool finalCaptureQueued = false;
    glm::vec3 target{0.0f};
    glm::vec3 startPosition{0.0f};
    float startYaw = 0.0f;
    float startPitch = 0.0f;
    float horizontalRadius = 0.0f;
    float verticalOffset = 0.0f;
    float startAngle = 0.0f;
    std::optional<nlohmann::json> pendingCapture;
    nlohmann::json captures = nlohmann::json::array();
    nlohmann::json rendererConfiguration = nlohmann::json::object();
    nlohmann::json capability = nlohmann::json::object();

    void Configure(Scene& scene, Camera& camera)
    {
        const int targetIndex = FindNamedObject(scene, targetName);
        if (targetIndex < 0)
        {
            throw std::runtime_error("Optical orbit target object not found: " + targetName);
        }

        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        scene.GetWorldBounds(targetIndex, boundsMin, boundsMax);
        target = 0.5f * (boundsMin + boundsMax);
        startPosition = camera.GetPosition();
        startYaw = camera.GetYaw();
        startPitch = camera.GetPitch();
        const glm::vec3 offset = startPosition - target;
        horizontalRadius = glm::length(glm::vec2(offset.x, offset.z));
        verticalOffset = offset.y;
        startAngle = std::atan2(offset.z, offset.x);
        if (horizontalRadius < 0.1f)
        {
            throw std::runtime_error("Optical orbit camera is too close to the target's vertical axis.");
        }

        SceneRenderer& renderer = scene.GetRenderer();
        DxrSettings& dxr = renderer.GetDxrSettings();
        ScreenSpaceEffects& effects = renderer.GetScreenSpaceEffects();
        dxr.SetEnabled(true);
        dxr.SetRenderingMode(RenderingMode::PathTraced);
        dxr.SetPtConvergenceMode(PtConvergenceMode::RealTime);
        dxr.SetPtDeterministicOpticalSplitEnabled(true);
        dxr.SetRestirDiTemporalEnabled(false);
        dxr.SetRestirGiInitialEnabled(false);
        dxr.SetRestirGiTemporalEnabled(false);
        dxr.SetRestirGiSpatialEnabled(false);
        const std::string mirrorChainOverride =
            ReadOptionalEnvironmentString("GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR");
        if (mirrorChainOverride == "0")
        {
            dxr.SetPtMirrorChainPsrEnabled(false);
        }
        else if (mirrorChainOverride == "1")
        {
            dxr.SetPtMirrorChainPsrEnabled(true);
        }
        else if (!mirrorChainOverride.empty())
        {
            throw std::runtime_error(
                "GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR must be 0 or 1.");
        }
        effects.SetAntiAliasingMode(captureMode.antiAliasing);
        effects.SetRayReconstruction(captureMode.rayReconstruction);
        renderer.SetRenderDebugMode(captureMode.debugMode);
        effects.InvalidateAllTemporalState();

        rendererConfiguration = {
            {"path_tracer", {
                {"rendering_mode", DxrSettings::RenderingModeToString(dxr.GetRenderingMode())},
                {"convergence_mode",
                 DxrSettings::PtConvergenceModeToString(dxr.GetPtConvergenceMode())},
                {"max_bounces", dxr.GetPtMaxBounces()},
                {"russian_roulette", dxr.IsPtRussianRouletteEnabled()},
                {"firefly_clamp", dxr.IsPtFireflyClampEnabled()},
                {"mirror_chain_psr", dxr.IsPtMirrorChainPsrEnabled()},
                {"psr_max_bounces", dxr.GetPtPsrMaxBounces()},
                {"psr_subpixel_threshold", dxr.GetPtPsrSubpixelThreshold()}}},
            {"reconstruction", {
                {"anti_aliasing_mode", static_cast<int>(effects.GetAntiAliasingMode())},
                {"ray_reconstruction", effects.GetRayReconstruction()},
                {"dlss_quality", static_cast<int>(effects.GetDlssPreset())},
                {"rr_preset", static_cast<int>(effects.GetRrPreset())},
                {"rr_bundle_mode", dxr.GetPtRrBundleMode()}}},
            {"viewport", {
                {"output_extent", {GfxContext::Get().GetWidth(), GfxContext::Get().GetHeight()}},
                {"render_extent", {effects.GetRenderWidth(), effects.GetRenderHeight()}}}},
        };
        capability = MakeCapabilityRecord(GfxContext::Get().GetDxrRuntimeSnapshot());

        camera.SetPosition(startPosition);
        camera.SetOrientationFromDirection(target - startPosition);
        warmupRemaining = warmupFrames;
        configured = true;
        EngineLog::Info(
            "optical-capture",
            "Configured mode=" + mode + " target=\"" + targetName + "\" warmup="
                + std::to_string(warmupFrames) + " orbit_frames=" + std::to_string(orbitFrames)
                + " stride=" + std::to_string(frameStride));
    }

    bool QueueCapture(const Camera& camera, const float angle, const int capturedOrbitFrame)
    {
        if (GfxContext::Get().HasPendingPresentedImageCapture())
        {
            return false;
        }
        if (!GfxContext::Get().RequestPresentedImageCapture())
        {
            throw std::runtime_error("Could not queue optical orbit presented-image capture.");
        }
        const glm::vec3 position = camera.GetPosition();
        pendingCapture = {
            {"capture_index", captureIndex},
            {"orbit_frame", capturedOrbitFrame},
            {"orbit_progress", static_cast<double>(capturedOrbitFrame) / static_cast<double>(orbitFrames)},
            {"angle_degrees", glm::degrees(angle)},
            {"camera_position", {position.x, position.y, position.z}},
            {"camera_yaw", camera.GetYaw()},
            {"camera_pitch", camera.GetPitch()},
        };
        return true;
    }

    void PrepareFrame(Scene& scene, Camera& camera)
    {
        if (!configured)
        {
            Configure(scene, camera);
        }
        if (complete)
        {
            return;
        }
        if (warmupRemaining > 0)
        {
            --warmupRemaining;
            return;
        }
        if (orbitFrame >= orbitFrames)
        {
            if (!finalCaptureQueued && !pendingCapture.has_value())
            {
                constexpr float kTwoPi = 6.28318530717958647692f;
                const int finalFrame = orbitFrames - 1;
                const float progress = static_cast<float>(finalFrame) / static_cast<float>(orbitFrames);
                const float angle = startAngle
                    + kTwoPi * static_cast<float>(revolutions) * progress;
                finalCaptureQueued = QueueCapture(camera, angle, finalFrame);
            }
            return;
        }

        constexpr float kTwoPi = 6.28318530717958647692f;
        const float progress = static_cast<float>(orbitFrame) / static_cast<float>(orbitFrames);
        const float angle = startAngle + kTwoPi * static_cast<float>(revolutions) * progress;
        const glm::vec3 position = target + glm::vec3(
            std::cos(angle) * horizontalRadius,
            verticalOffset,
            std::sin(angle) * horizontalRadius);
        camera.SetPosition(position);
        camera.SetOrientationFromDirection(target - position);

        if (orbitFrame % frameStride == 0 || orbitFrame + 1 == orbitFrames)
        {
            const bool queued = QueueCapture(camera, angle, orbitFrame);
            if (orbitFrame + 1 == orbitFrames)
            {
                finalCaptureQueued = queued;
            }
        }
        ++orbitFrame;
    }

    void WriteCapture(GfxContext::PresentedImageCapture image)
    {
        if (!pendingCapture.has_value())
        {
            throw std::runtime_error("Optical image readback completed without capture metadata.");
        }
        std::ostringstream stem;
        stem << "frame-" << std::setfill('0') << std::setw(4) << captureIndex;
        const std::filesystem::path rgbaPath = outputDirectory / (stem.str() + ".rgba");
        const std::filesystem::path metadataPath = outputDirectory / (stem.str() + ".json");
        std::ofstream rgba(rgbaPath, std::ios::binary | std::ios::trunc);
        if (!rgba)
        {
            throw std::runtime_error("Could not write optical capture: " + rgbaPath.string());
        }
        rgba.write(
            reinterpret_cast<const char*>(image.rgba8.data()),
            static_cast<std::streamsize>(image.rgba8.size()));
        if (!rgba)
        {
            throw std::runtime_error("Could not finish optical capture: " + rgbaPath.string());
        }

        nlohmann::json metadata = std::move(*pendingCapture);
        metadata["record_type"] = "optical_orbit_frame";
        metadata["mode"] = mode;
        metadata["extent"] = {image.width, image.height};
        metadata["format"] = "rgba8";
        metadata["rgba"] = rgbaPath.filename().generic_string();
        std::ofstream metadataOutput(metadataPath, std::ios::trunc);
        metadataOutput << metadata.dump(2) << '\n';
        if (!metadataOutput)
        {
            throw std::runtime_error("Could not write optical metadata: " + metadataPath.string());
        }
        captures.push_back(metadata);
        pendingCapture.reset();
        ++captureIndex;
    }

    void Finish()
    {
        if (complete)
        {
            return;
        }
        const nlohmann::json manifest = {
            {"record_type", "optical_orbit_capture"},
            {"schema_version", 2},
            {"mode", mode},
            {"target", targetName},
            {"target_world", {target.x, target.y, target.z}},
            {"warmup_frames", warmupFrames},
            {"orbit_frames", orbitFrames},
            {"revolutions", revolutions},
            {"frame_stride", frameStride},
            {"start_camera", {
                {"position", {startPosition.x, startPosition.y, startPosition.z}},
                {"yaw", startYaw},
                {"pitch", startPitch}}},
            {"renderer", rendererConfiguration},
            {"capability", capability},
            {"timing_evidence",
             "Use the paired fixed-pose benchmark timing CSV; optical readback and GPU profiler "
             "slices are asynchronous and are not falsely attributed to individual orbit frames."},
            {"captures", captures},
        };
        std::ofstream output(outputDirectory / "capture.json", std::ios::trunc);
        output << manifest.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error("Could not write optical orbit capture manifest.");
        }
        complete = true;
        EngineLog::Info(
            "optical-capture",
            "Optical orbit complete mode=" + mode + " captures=" + std::to_string(captureIndex));
    }
};

AutomatedOpticalOrbitCapture::AutomatedOpticalOrbitCapture(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

AutomatedOpticalOrbitCapture::~AutomatedOpticalOrbitCapture() = default;

std::unique_ptr<AutomatedOpticalOrbitCapture> AutomatedOpticalOrbitCapture::CreateFromEnvironment()
{
    const std::string output = ReadOptionalEnvironmentString("GAME_ENGINE_OPTICAL_CAPTURE_OUTPUT");
    if (output.empty())
    {
        return nullptr;
    }
    const std::string mode = ReadOptionalEnvironmentString("GAME_ENGINE_OPTICAL_CAPTURE_MODE");
    if (mode.empty())
    {
        throw std::runtime_error(
            "GAME_ENGINE_OPTICAL_CAPTURE_MODE is required with GAME_ENGINE_OPTICAL_CAPTURE_OUTPUT.");
    }

    auto impl = std::make_unique<Impl>();
    impl->outputDirectory = output;
    impl->mode = mode;
    impl->captureMode = ResolveOpticalCaptureMode(mode);
    impl->targetName = ReadOptionalEnvironmentString("GAME_ENGINE_OPTICAL_CAPTURE_TARGET");
    if (impl->targetName.empty())
    {
        impl->targetName = "glass sphere";
    }
    impl->warmupFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_OPTICAL_CAPTURE_WARMUP_FRAMES", 120);
    impl->orbitFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_OPTICAL_CAPTURE_ORBIT_FRAMES", 180);
    impl->revolutions = ReadPositiveEnvironmentInt("GAME_ENGINE_OPTICAL_CAPTURE_REVOLUTIONS", 1);
    impl->frameStride = ReadPositiveEnvironmentInt("GAME_ENGINE_OPTICAL_CAPTURE_FRAME_STRIDE", 12);
    if (impl->frameStride > impl->orbitFrames)
    {
        throw std::runtime_error("GAME_ENGINE_OPTICAL_CAPTURE_FRAME_STRIDE cannot exceed orbit frames.");
    }
    std::error_code error;
    std::filesystem::create_directories(impl->outputDirectory, error);
    if (error)
    {
        throw std::runtime_error("Could not create optical capture directory: " + error.message());
    }
    return std::unique_ptr<AutomatedOpticalOrbitCapture>(
        new AutomatedOpticalOrbitCapture(std::move(impl)));
}

void AutomatedOpticalOrbitCapture::PrepareFrame(
    const bool sceneReady,
    Scene& scene,
    Camera& camera)
{
    if (sceneReady)
    {
        m_impl->PrepareFrame(scene, camera);
    }
}

bool AutomatedOpticalOrbitCapture::ObserveFrame()
{
    if (!m_impl->configured || m_impl->complete)
    {
        return m_impl->complete;
    }
    if (m_impl->pendingCapture.has_value())
    {
        GfxContext::PresentedImageCapture image;
        if (GfxContext::Get().TryConsumePresentedImageCapture(image))
        {
            m_impl->WriteCapture(std::move(image));
        }
    }
    if (m_impl->orbitFrame >= m_impl->orbitFrames
        && m_impl->finalCaptureQueued
        && !m_impl->pendingCapture.has_value()
        && !GfxContext::Get().HasPendingPresentedImageCapture())
    {
        m_impl->Finish();
    }
    return m_impl->complete;
}
