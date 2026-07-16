#include "app/core/AutomatedBenchmarkCapture.h"

#include "engine/platform/EngineLog.h"
#include "engine/rhi/GfxContext.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
}

AutomatedBenchmarkCapture::AutomatedBenchmarkCapture(
    std::string outputPath,
    std::string imageOutputPath,
    std::string comparisonMode,
    const int warmupSeconds,
    const int warmupFrames,
    const int sampleFrames)
    : m_outputPath(std::move(outputPath))
    , m_imageOutputPath(std::move(imageOutputPath))
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
    const bool s0p5Capture = std::getenv("GAME_ENGINE_S0P5_CAPTURE") != nullptr;
    if (s0p5Capture && imageOutputPath.empty())
    {
        throw std::runtime_error("GAME_ENGINE_S0P5_CAPTURE requires GAME_ENGINE_CAPTURE_IMAGE_OUTPUT.");
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
            rawOutput, imageOutputPath, comparisonMode, warmupSeconds, warmupFrames, sampleFrames));
}

bool AutomatedBenchmarkCapture::ObserveFrame(
    const bool sceneReady,
    const std::vector<GpuProfiler::Entry>& gpuTimings,
    const ApplicationFrameDiagnostics& applicationTimings)
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
