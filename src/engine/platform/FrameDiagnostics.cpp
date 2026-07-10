#include "engine/platform/FrameDiagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    bool g_enabled = false;
    bool g_enabledInitialized = false;

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
}
