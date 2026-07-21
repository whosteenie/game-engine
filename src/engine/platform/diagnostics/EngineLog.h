#pragma once

#include "engine/platform/system/ExceptionMessage.h"

#include <exception>
#include <string>

// Persistent engine logging to diagnostics/engine.log and stderr.
// Errors and warnings are always logged. Info lines require GAME_ENGINE_LOG=1.
namespace EngineLog
{
    void EnsureLogDirectoryExists();

    void Info(const char* category, const std::string& message);
    void Warn(const char* category, const std::string& message);
    void Error(const char* category, const std::string& message);

    // Always logged (load/render breadcrumbs). Not gated by GAME_ENGINE_LOG.
    void Breadcrumb(const char* category, const std::string& message);

    void LogExceptionImpl(
        const char* category,
        const char* phase,
        const char* what,
        const char* mangledTypeName);

    template <typename ExceptionType>
    void LogException(const char* category, const char* phase, const ExceptionType& exception)
    {
        const char* what = nullptr;
        try
        {
            what = exception.what();
        }
        catch (...)
        {
            what = nullptr;
        }

        LogExceptionImpl(category, phase, what, typeid(exception).name());
    }

    // Logs phase + message without throwing. Use at catch sites before returning errors to UI.
    void LogFailure(const char* category, const char* phase, const std::string& message);
}
