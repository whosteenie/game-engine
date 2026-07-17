#include "app/core/AutomatedBenchmarkCapture.h"

#include "engine/camera/Camera.h"
#include "engine/platform/CaptureManifest.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rhi/GfxContext.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
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
                                 dxrSettings.IsPtDeterministicOpticalSplitEnabled()}}},
            {"restir", {{"di_candidates", dxrSettings.GetRestirDiCandidateCount()},
                          {"di_temporal", dxrSettings.IsRestirDiTemporalEnabled()},
                          {"gi_initial", dxrSettings.IsRestirGiInitialEnabled()},
                          {"gi_temporal", dxrSettings.IsRestirGiTemporalEnabled()},
                          {"gi_spatial", dxrSettings.IsRestirGiSpatialEnabled()},
                          {"diagnostic_mode", static_cast<int>(dxrSettings.GetRestirGiSpatialDiagnosticMode())}}},
            {"reconstruction", {{"feature", screenSpaceEffects.GetRayReconstruction() ? "ray_reconstruction" : "none"},
                                  {"quality", static_cast<int>(screenSpaceEffects.GetDlssPreset())},
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
