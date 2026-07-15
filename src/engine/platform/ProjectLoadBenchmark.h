#pragma once

#include <chrono>
#include <string>

// Opt-in, process-local timing for project opening. It is intentionally controlled by an
// environment variable so normal editor opens do not acquire benchmark file I/O or automatic
// close behavior. The PowerShell driver launches a fresh process for each sample.
namespace ProjectLoadBenchmark
{
    // Enables capture when GAME_ENGINE_PROJECT_LOAD_BENCHMARK_OUTPUT names a JSON file.
    void StartFromEnvironment();
    bool IsActive();
    bool IsComplete();

    // Records an instantaneous milestone relative to StartFromEnvironment().
    void Mark(const char* name);

    // Writes the completed capture. Safe to call more than once.
    void Complete();
    void Fail(const std::string& reason);

    class ScopedPhase
    {
    public:
        explicit ScopedPhase(const char* name);
        ~ScopedPhase();

        ScopedPhase(const ScopedPhase&) = delete;
        ScopedPhase& operator=(const ScopedPhase&) = delete;

    private:
        const char* m_name = nullptr;
        std::chrono::steady_clock::time_point m_start{};
        bool m_active = false;
    };
}
