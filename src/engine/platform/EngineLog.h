#pragma once

#include <string>

// Persistent engine logging to diagnostics/engine.log and stderr.
// Errors and warnings are always logged. Info lines require GAME_ENGINE_LOG=1.
namespace EngineLog
{
    void EnsureLogDirectoryExists();

    void Info(const char* category, const std::string& message);
    void Warn(const char* category, const std::string& message);
    void Error(const char* category, const std::string& message);

    void LogException(const char* category, const char* phase, const std::exception& exception);

    // Logs phase + message without throwing. Use at catch sites before returning errors to UI.
    void LogFailure(const char* category, const char* phase, const std::string& message);
}
