#pragma once

#include "app/panels/PerformancePanel.h"
#include "engine/rhi/GpuProfiler.h"

#include <fstream>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

class Camera;
class DxrSettings;
class ScreenSpaceEffects;

// Opt-in machine-readable capture of the existing unsmoothed D3D12 timestamp scopes. It is
// deliberately controlled only by environment variables so normal editor sessions do not acquire
// benchmark I/O or automatic-close behavior.
class AutomatedBenchmarkCapture
{
public:
    static std::unique_ptr<AutomatedBenchmarkCapture> CreateFromEnvironment();

    // Returns true only after the configured sample window has been written completely.
    bool ObserveFrame(
        bool sceneReady,
        const std::vector<GpuProfiler::Entry>& gpuTimings,
        const ApplicationFrameDiagnostics& applicationTimings,
        const std::string& projectFilePath,
        const std::string& projectRootDirectory,
        const Camera& camera,
        const DxrSettings& dxrSettings,
        const ScreenSpaceEffects& screenSpaceEffects,
        int renderDebugMode);

private:
    AutomatedBenchmarkCapture(
        std::string outputPath,
        std::string imageOutputPath,
        std::string manifestOutputPath,
        std::string manifestInputPath,
        std::string revision,
        std::string comparisonMode,
        int warmupSeconds,
        int warmupFrames,
        int sampleFrames);

    std::string m_outputPath;
    std::string m_imageOutputPath;
    std::string m_manifestOutputPath;
    std::string m_manifestInputPath;
    std::string m_revision;
    std::string m_comparisonMode;
    int m_warmupSeconds = 0;
    int m_warmupFrames = 0;
    int m_sampleFrames = 0;
    int m_capturedFrames = 0;
    bool m_started = false;
    bool m_manifestWritten = false;
    bool m_complete = false;
    bool m_imageCaptureRequested = false;
    std::chrono::steady_clock::time_point m_readyTime{};
    std::chrono::steady_clock::time_point m_imageCaptureRequestTime{};
    std::unique_ptr<std::ofstream> m_output;
};
