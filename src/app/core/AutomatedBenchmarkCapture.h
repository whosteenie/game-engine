#pragma once

#include "app/panels/PerformancePanel.h"
#include "engine/rhi/GpuProfiler.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
        const ApplicationFrameDiagnostics& applicationTimings);

private:
    AutomatedBenchmarkCapture(std::string outputPath, int warmupFrames, int sampleFrames);

    std::string m_outputPath;
    int m_warmupFrames = 0;
    int m_sampleFrames = 0;
    int m_capturedFrames = 0;
    bool m_started = false;
    bool m_complete = false;
    std::unique_ptr<std::ofstream> m_output;
};
