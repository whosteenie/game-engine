#pragma once

#include <cstdint>

namespace FrameDiagnostics
{
    bool IsEnabled();

    void Log(const char* message);

    void LogFenceWait(std::uint64_t waitedFenceValue, std::uint64_t completedValue, long long elapsedMs);

    void LogPhase(const char* phase);
}
