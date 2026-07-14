#include "app/core/AutomatedBenchmarkCapture.h"

#include "engine/platform/EngineLog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

    float FindGpuTiming(const std::vector<GpuProfiler::Entry>& timings, const char* name)
    {
        const auto found = std::find_if(
            timings.begin(),
            timings.end(),
            [name](const GpuProfiler::Entry& entry) { return entry.name == name; });
        return found != timings.end() ? found->milliseconds : -1.0f;
    }
}

AutomatedBenchmarkCapture::AutomatedBenchmarkCapture(
    std::string outputPath,
    const int warmupFrames,
    const int sampleFrames)
    : m_outputPath(std::move(outputPath))
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

    const int warmupFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_BENCHMARK_WARMUP_FRAMES", 120);
    const int sampleFrames = ReadPositiveEnvironmentInt("GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES", 300);
    return std::unique_ptr<AutomatedBenchmarkCapture>(
        new AutomatedBenchmarkCapture(rawOutput, warmupFrames, sampleFrames));
}

bool AutomatedBenchmarkCapture::ObserveFrame(
    const bool sceneReady,
    const std::vector<GpuProfiler::Entry>& gpuTimings,
    const ApplicationFrameDiagnostics& applicationTimings)
{
    if (m_complete || !sceneReady || gpuTimings.empty())
    {
        return m_complete;
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
        EngineLog::Info(
            "benchmark",
            "Timestamp capture armed: warmup=" + std::to_string(m_warmupFrames)
                + ", samples=" + std::to_string(m_sampleFrames));
    }

    if (m_warmupFrames > 0)
    {
        --m_warmupFrames;
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
    m_complete = true;
    EngineLog::Info("benchmark", "Timestamp capture complete: " + m_outputPath);
    return true;
}
