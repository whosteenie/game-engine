#include "engine/platform/tooling/ProjectLoadBenchmark.h"

#include "engine/platform/diagnostics/EngineLog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    struct TimedEntry
    {
        std::string name;
        double milliseconds = 0.0;
    };

    struct CaptureState
    {
        bool active = false;
        bool complete = false;
        bool succeeded = true;
        std::string failureReason;
        std::string outputPath;
        std::chrono::steady_clock::time_point startTime{};
        std::vector<TimedEntry> milestones;
        std::vector<TimedEntry> phases;
    };

    CaptureState g_state;

    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }

    std::string EscapeJson(const std::string& text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char character : text)
        {
            switch (character)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(character); break;
            }
        }
        return escaped;
    }

    void WriteEntries(
        std::ofstream& output,
        const char* propertyName,
        const std::vector<TimedEntry>& entries,
        const char* valueName)
    {
        output << "  \"" << propertyName << "\": [\n";
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const TimedEntry& entry = entries[index];
            output << "    {\"name\": \"" << EscapeJson(entry.name) << "\", \""
                   << valueName << "\": " << entry.milliseconds << "}";
            output << (index + 1 < entries.size() ? ",\n" : "\n");
        }
        output << "  ]";
    }

    void WriteResult()
    {
        if (!g_state.active || g_state.complete)
        {
            return;
        }

        const std::filesystem::path outputPath(g_state.outputPath);
        std::error_code error;
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path(), error);
        }
        if (error)
        {
            throw std::runtime_error(
                "Could not create project-load benchmark output directory: " + error.message());
        }

        std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open project-load benchmark output: " + g_state.outputPath);
        }

        const double totalMilliseconds = ElapsedMilliseconds(g_state.startTime);
        output.setf(std::ios::fixed);
        output << std::setprecision(3);
        output << "{\n";
        output << "  \"status\": \"" << (g_state.succeeded ? "complete" : "failed") << "\",\n";
        output << "  \"total_ms\": " << totalMilliseconds << ",\n";
        output << "  \"failure_reason\": \"" << EscapeJson(g_state.failureReason) << "\",\n";
        WriteEntries(output, "milestones", g_state.milestones, "time_ms");
        output << ",\n";
        WriteEntries(output, "phases", g_state.phases, "duration_ms");
        output << "\n}\n";
        output.close();

        g_state.complete = true;
        EngineLog::Info("benchmark", "Project-load capture complete: " + g_state.outputPath);
    }
}

namespace ProjectLoadBenchmark
{
    void StartFromEnvironment()
    {
        if (g_state.active)
        {
            return;
        }

        const char* rawOutput = std::getenv("GAME_ENGINE_PROJECT_LOAD_BENCHMARK_OUTPUT");
        if (rawOutput == nullptr || rawOutput[0] == '\0')
        {
            return;
        }

        g_state = {};
        g_state.active = true;
        g_state.outputPath = rawOutput;
        g_state.startTime = std::chrono::steady_clock::now();
        Mark("application.run.begin");
    }

    bool IsActive()
    {
        return g_state.active && !g_state.complete;
    }

    bool IsComplete()
    {
        return g_state.complete;
    }

    void Mark(const char* name)
    {
        if (!IsActive() || name == nullptr)
        {
            return;
        }

        g_state.milestones.push_back({name, ElapsedMilliseconds(g_state.startTime)});
    }

    void Complete()
    {
        if (!IsActive())
        {
            return;
        }

        Mark("benchmark.complete");
        WriteResult();
    }

    void Fail(const std::string& reason)
    {
        if (!IsActive())
        {
            return;
        }

        g_state.succeeded = false;
        g_state.failureReason = reason;
        Mark("benchmark.failed");
        WriteResult();
    }

    ScopedPhase::ScopedPhase(const char* name)
        : m_name(name), m_active(IsActive() && name != nullptr)
    {
        if (m_active)
        {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ScopedPhase::~ScopedPhase()
    {
        if (!m_active || !IsActive())
        {
            return;
        }

        const double duration = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - m_start)
                                    .count();
        g_state.phases.push_back({m_name, duration});
    }
}
