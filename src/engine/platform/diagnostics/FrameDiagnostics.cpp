#include "engine/platform/diagnostics/FrameDiagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <string>

namespace
{
    bool g_enabled = false;
    bool g_enabledInitialized = false;
    std::uint64_t g_applicationFrameSerial = 0;
    std::uint64_t g_activeApplicationFrameSerial = 0;
    std::uint32_t g_frameEventOrdinal = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> g_viewportEventOrdinals;
    std::unordered_map<std::string, std::uint64_t> g_historyGenerations;

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

    void LogHistoryEvent(
        const std::uint32_t viewportId,
        const char* owner,
        const char* event,
        const char* producer,
        const char* guideVersion,
        const char* feature,
        const char* quality,
        const int renderWidth,
        const int renderHeight,
        const int viewportWidth,
        const int viewportHeight,
        const bool cameraCut,
        const bool diagnosticInput,
        const std::uint32_t reasonBits)
    {
        if (!QueryEnabled())
        {
            return;
        }

        const char* const safeOwner = owner != nullptr ? owner : "unknown";
        const char* const safeEvent = event != nullptr ? event : "unknown";
        const std::string key = std::to_string(viewportId) + ":" + safeOwner;
        std::uint64_t& generation = g_historyGenerations[key];
        if (std::strcmp(safeEvent, "request") == 0)
        {
            ++generation;
        }
        std::fprintf(
            stderr,
            "[frame] history-trace app_serial=%llu viewport=%u owner=%s event=%s generation=%llu "
            "producer=%s guide=%s feature=%s quality=%s render=%dx%d viewport_extent=%dx%d "
            "camera_cut=%u diagnostic=%u reason_bits=0x%X\n",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial), viewportId, safeOwner,
            safeEvent, static_cast<unsigned long long>(generation),
            producer != nullptr ? producer : "unknown", guideVersion != nullptr ? guideVersion : "unknown",
            feature != nullptr ? feature : "unknown", quality != nullptr ? quality : "unknown",
            renderWidth, renderHeight, viewportWidth, viewportHeight,
            cameraCut ? 1u : 0u, diagnosticInput ? 1u : 0u, reasonBits);
        std::fflush(stderr);
    }

    void LogHistoryCompatibility(
        const std::uint32_t viewportId,
        const char* event,
        const char* producer,
        const char* guideProducer,
        const std::uint32_t guideVersion,
        const char* feature,
        const char* quality,
        const std::uint32_t qualityVersion,
        const int renderWidth,
        const int renderHeight,
        const int outputWidth,
        const int outputHeight,
        const bool cameraValid,
        const bool cameraCut,
        const std::uint32_t opticalSceneVersion,
        const std::uint32_t opticalMotionVersion,
        const std::uint32_t diagnosticSignal,
        const std::uint32_t reasonBits,
        const std::uint32_t ownerBits)
    {
        if (!QueryEnabled())
        {
            return;
        }

        std::fprintf(
            stderr,
            "[frame] history-key app_serial=%llu viewport=%u event=%s producer=%s guide=%s "
            "guide_version=%u feature=%s quality=%s quality_version=%u render=%dx%d output=%dx%d "
            "camera_valid=%u camera_cut=%u optical_scene=%u optical_motion=%u "
            "diagnostic=0x%X reason_bits=0x%X owner_bits=0x%X\n",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial),
            viewportId,
            event != nullptr ? event : "unknown",
            producer != nullptr ? producer : "unknown",
            guideProducer != nullptr ? guideProducer : "unknown",
            guideVersion,
            feature != nullptr ? feature : "unknown",
            quality != nullptr ? quality : "unknown",
            qualityVersion,
            renderWidth,
            renderHeight,
            outputWidth,
            outputHeight,
            cameraValid ? 1u : 0u,
            cameraCut ? 1u : 0u,
            opticalSceneVersion,
            opticalMotionVersion,
            diagnosticSignal,
            reasonBits,
            ownerBits);
        std::fflush(stderr);
    }

    void LogReconstructionJitter(
        const std::uint32_t viewportId,
        const char* event,
        const char* feature,
        const char* quality,
        const std::uint32_t period,
        const std::uint32_t phase,
        const bool previousValid,
        const std::uint32_t previousPhase,
        const bool startsNewHistory,
        const float xNdc,
        const float yNdc)
    {
        if (!QueryEnabled())
        {
            return;
        }

        std::fprintf(
            stderr,
            "[frame] jitter-trace app_serial=%llu viewport=%u event=%s feature=%s quality=%s "
            "period=%u phase=%u previous_valid=%u previous_phase=",
            static_cast<unsigned long long>(g_activeApplicationFrameSerial),
            viewportId,
            event != nullptr ? event : "unknown",
            feature != nullptr ? feature : "unknown",
            quality != nullptr ? quality : "unknown",
            period,
            phase,
            previousValid ? 1u : 0u);
        if (previousValid)
        {
            std::fprintf(stderr, "%u", previousPhase);
        }
        else
        {
            std::fputs("none", stderr);
        }
        std::fprintf(
            stderr,
            " reset=%u jitter_ndc=(%.9g,%.9g)\n",
            startsNewHistory ? 1u : 0u,
            static_cast<double>(xNdc),
            static_cast<double>(yNdc));
        std::fflush(stderr);
    }
}
