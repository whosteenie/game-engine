#pragma once

#include "engine/platform/EngineLog.h"

#include <cctype>
#include <cstring>
#include <string>

namespace DxrTraceDetail
{
inline bool& TrustMode()
{
    static bool trustMode = false;
    return trustMode;
}

inline bool (&BreadcrumbOnceFlags())[7]
{
    static bool loggedKeys[7]{};
    return loggedKeys;
}

inline bool (&ErrorOnceFlags())[7]
{
    static bool loggedKeys[7]{};
    return loggedKeys;
}

inline bool ContainsIgnoreCase(const std::string& text, const char* token)
{
    if (token == nullptr || *token == '\0')
    {
        return false;
    }

    const std::size_t tokenLength = std::strlen(token);
    if (text.size() < tokenLength)
    {
        return false;
    }

    for (std::size_t index = 0; index + tokenLength <= text.size(); ++index)
    {
        bool match = true;
        for (std::size_t tokenIndex = 0; tokenIndex < tokenLength; ++tokenIndex)
        {
            const unsigned char left = static_cast<unsigned char>(text[index + tokenIndex]);
            const unsigned char right = static_cast<unsigned char>(token[tokenIndex]);
            if (std::tolower(left) != std::tolower(right))
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return true;
        }
    }

    return false;
}

// Failures, skips, and other attention-worthy DXR messages are always logged.
inline bool BreadcrumbRequiresAttention(const std::string& step)
{
    static const char* attentionTokens[] = {
        "failed",
        "unavailable",
        "missing",
        "skipped",
    };

    for (const char* token : attentionTokens)
    {
        if (ContainsIgnoreCase(step, token))
        {
            return true;
        }
    }

    return false;
}

inline int FindBreadcrumbOnceKeyIndex(const char* key)
{
    static const char* keys[] = {
        "as-skipped",
        "render-ensure-scene",
        "render-smoke-dispatch",
        "render-smoke-debug-srv",
        "dispatch-ensure-output",
        "dispatch-smoke-failure",
        "dispatch-ensure-output-error",
    };

    for (int index = 0; index < static_cast<int>(sizeof(keys) / sizeof(keys[0])); ++index)
    {
        if (std::strcmp(key, keys[index]) == 0)
        {
            return index;
        }
    }

    return -1;
}
} // namespace DxrTraceDetail

// DXR breadcrumbs (always logged for failures/skips; success paths suppressed after trust mode).
inline void DxrBreadcrumb(const std::string& step)
{
    if (DxrTraceDetail::TrustMode() && !DxrTraceDetail::BreadcrumbRequiresAttention(step))
    {
        return;
    }

    EngineLog::Breadcrumb("dxr", step);
}

// After the first verified DXR dispatch attempt, suppress per-frame success breadcrumbs.
inline void DxrEnableTrustMode()
{
    if (DxrTraceDetail::TrustMode())
    {
        return;
    }

    DxrTraceDetail::TrustMode() = true;
    EngineLog::Breadcrumb("dxr", "=== dxr steady-state (breadcrumbs suppressed) ===");
}

// Logs once per key until ResetDxrBreadcrumbOnceFlags() is called.
inline void DxrBreadcrumbOnce(const char* key, const std::string& step)
{
    const int keyIndex = DxrTraceDetail::FindBreadcrumbOnceKeyIndex(key);
    if (keyIndex >= 0)
    {
        bool (&loggedKeys)[7] = DxrTraceDetail::BreadcrumbOnceFlags();
        if (loggedKeys[keyIndex])
        {
            return;
        }

        loggedKeys[keyIndex] = true;
    }

    DxrBreadcrumb(step);
}

inline void ResetDxrBreadcrumbOnceFlags()
{
    bool (&breadcrumbKeys)[7] = DxrTraceDetail::BreadcrumbOnceFlags();
    for (bool& logged : breadcrumbKeys)
    {
        logged = false;
    }

    bool (&errorKeys)[7] = DxrTraceDetail::ErrorOnceFlags();
    for (bool& logged : errorKeys)
    {
        logged = false;
    }

    DxrTraceDetail::TrustMode() = false;
}

// Logs to [error] [dxr-dispatch] once per key until ResetDxrBreadcrumbOnceFlags().
inline void DxrLogErrorOnce(const char* key, const std::string& message)
{
    const int keyIndex = DxrTraceDetail::FindBreadcrumbOnceKeyIndex(key);
    if (keyIndex >= 0)
    {
        bool (&loggedKeys)[7] = DxrTraceDetail::ErrorOnceFlags();
        if (loggedKeys[keyIndex])
        {
            return;
        }

        loggedKeys[keyIndex] = true;
    }

    EngineLog::Error("dxr-dispatch", message);
}
