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

    // S0-P3: observational history lifecycle trace.  The implementation owns the generation
    // counter; this API never participates in a reset decision or changes a history value.
    void LogHistoryEvent(
        std::uint32_t viewportId,
        const char* owner,
        const char* event,
        const char* producer,
        const char* guideVersion,
        const char* feature,
        const char* quality,
        int renderWidth,
        int renderHeight,
        int viewportWidth,
        int viewportHeight,
        bool cameraCut,
        bool diagnosticInput,
        std::uint32_t reasonBits);
}
