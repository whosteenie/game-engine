#include "engine/platform/FrameDiagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace
{
    bool g_enabled = false;
    bool g_enabledInitialized = false;
    std::uint64_t g_applicationFrameSerial = 0;
    std::uint64_t g_activeApplicationFrameSerial = 0;
    std::uint32_t g_frameEventOrdinal = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> g_viewportEventOrdinals;

    bool QueryEnabled()
    {
        if (!g_enabledInitialized)
        {
            g_enabledInitialized = true;
            const char* value = std::getenv("GAME_ENGINE_FRAME_DEBUG");
            g_enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        }

        return g_enabled;
    }
}

namespace FrameDiagnostics
{
    bool IsEnabled()
    {
        return QueryEnabled();
    }

    void Log(const char* message)
    {
        if (!QueryEnabled() || message == nullptr)
        {
            return;
        }

        std::fprintf(stderr, "[frame] %s\n", message);
        std::fflush(stderr);
    }

    void LogFenceWait(
        const std::uint64_t waitedFenceValue,
        const std::uint64_t completedValue,
        const long long elapsedMs)
    {
        if (!QueryEnabled())
        {
            return;
        }

        std::fprintf(
            stderr,
            "[frame] fence-wait target=%llu completed=%llu elapsedMs=%lld\n",
            static_cast<unsigned long long>(waitedFenceValue),
            static_cast<unsigned long long>(completedValue),
            elapsedMs);
        std::fflush(stderr);
    }

    void LogPhase(const char* phase)
    {
        if (!QueryEnabled() || phase == nullptr)
        {
            return;
        }

        std::fprintf(stderr, "[frame] phase=%s\n", phase);
        std::fflush(stderr);
    }

    void BeginApplicationFrame()
    {
        if (!QueryEnabled())
        {
            return;
        }

        g_activeApplicationFrameSerial = ++g_applicationFrameSerial;
        g_frameEventOrdinal = 0;
        g_viewportEventOrdinals.clear();
        std::fprintf(
            stderr,
            "[frame] application-frame serial=%llu boundary=begin\n",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial));
        std::fflush(stderr);
    }

    void EndApplicationFrame()
    {
        if (!QueryEnabled() || g_activeApplicationFrameSerial == 0)
        {
            return;
        }

        std::fprintf(
            stderr,
            "[frame] application-frame serial=%llu boundary=end evaluations=%u\n",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial),
            g_frameEventOrdinal);
        std::fflush(stderr);
        g_activeApplicationFrameSerial = 0;
    }

    void LogDlssEvent(
        const std::uint32_t viewportId,
        const char* feature,
        const char* quality,
        const char* outcome,
        const char* reason,
        const bool hasSubmissionIndex,
        const std::uint64_t submissionIndex,
        const bool hasToken,
        const std::uint32_t token)
    {
        if (!QueryEnabled())
        {
            return;
        }

        const std::uint32_t frameOrdinal = ++g_frameEventOrdinal;
        const std::uint32_t viewportOrdinal = ++g_viewportEventOrdinals[viewportId];
        std::fprintf(
            stderr,
            "[frame] dlss-trace app_serial=%llu order=%u viewport=%u viewport_order=%u "
            "feature=%s quality=%s outcome=%s reason=%s submission_index=",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial),
            frameOrdinal,
            viewportId,
            viewportOrdinal,
            feature != nullptr ? feature : "unknown",
            quality != nullptr ? quality : "unknown",
            outcome != nullptr ? outcome : "unknown",
            reason != nullptr ? reason : "none");
        if (hasSubmissionIndex)
        {
            std::fprintf(stderr, "%llu", static_cast<unsigned long long>(submissionIndex));
        }
        else
        {
            std::fputs("none", stderr);
        }
        std::fputs(" token=", stderr);
        if (hasToken)
        {
            std::fprintf(stderr, "%u", token);
        }
        else
        {
            std::fputs("none", stderr);
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
}
