#pragma once

#include <cstdint>

namespace FrameDiagnostics
{
    bool IsEnabled();

    void Log(const char* message);

    void LogFenceWait(std::uint64_t waitedFenceValue, std::uint64_t completedValue, long long elapsedMs);

    void LogPhase(const char* phase);

    // S0-P2: diagnostics-only ownership context for the synchronous application render span.
    // These calls are inert unless GAME_ENGINE_FRAME_DEBUG is enabled and never feed rendering.
    void BeginApplicationFrame();
    void EndApplicationFrame();
    void LogDlssEvent(
        std::uint32_t viewportId,
        const char* feature,
        const char* quality,
        const char* outcome,
        const char* reason,
        bool hasSubmissionIndex,
        std::uint64_t submissionIndex,
        bool hasToken,
        std::uint32_t token);
}
