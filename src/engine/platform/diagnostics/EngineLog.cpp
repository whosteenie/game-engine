#include "engine/platform/diagnostics/EngineLog.h"

#include "engine/platform/system/ExceptionMessage.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace
{
    std::mutex g_logMutex;
    bool g_infoEnabled = false;
    bool g_infoEnabledInitialized = false;

    bool InfoLoggingEnabled()
    {
        if (!g_infoEnabledInitialized)
        {
            g_infoEnabledInitialized = true;
            const char* value = std::getenv("GAME_ENGINE_LOG");
            g_infoEnabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        }

        return g_infoEnabled;
    }

    fs::path GetLogDirectory()
    {
        return fs::path("diagnostics");
    }

    fs::path GetLogFilePath()
    {
        return GetLogDirectory() / "engine.log";
    }

    void WriteLine(const char* level, const char* category, const std::string& message)
    {
        const std::string safeMessage = SanitizeLogText(message, "Unknown error");

        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        char timeBuffer[32]{};
        std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", std::localtime(&nowTime));

        const char* safeCategory = category != nullptr ? category : "engine";
        std::fprintf(
            stderr,
            "[%s] [%s] [%s] %s\n",
            timeBuffer,
            level,
            safeCategory,
            safeMessage.c_str());
        std::fflush(stderr);

        std::lock_guard<std::mutex> lock(g_logMutex);
        std::ofstream output(GetLogFilePath(), std::ios::app);
        if (!output)
        {
            return;
        }

        output << timeBuffer << " [" << level << "] [" << safeCategory << "] " << safeMessage << '\n';
    }
}

namespace EngineLog
{
    void EnsureLogDirectoryExists()
    {
        std::error_code error;
        fs::create_directories(GetLogDirectory(), error);
    }

    void Info(const char* category, const std::string& message)
    {
        if (!InfoLoggingEnabled())
        {
            return;
        }

        EnsureLogDirectoryExists();
        WriteLine("info", category, message);
    }

    void Warn(const char* category, const std::string& message)
    {
        EnsureLogDirectoryExists();
        WriteLine("warn", category, message);
    }

    void Error(const char* category, const std::string& message)
    {
        EnsureLogDirectoryExists();
        WriteLine("error", category, message);
    }

    void Breadcrumb(const char* category, const std::string& message)
    {
        EnsureLogDirectoryExists();
        WriteLine(category != nullptr ? category : "trace", "breadcrumb", message);
    }

    void LogExceptionImpl(
        const char* category,
        const char* phase,
        const char* what,
        const char* mangledTypeName)
    {
        const std::string message = FormatExceptionContextImpl(phase, what, mangledTypeName);
        Error(category, message);
    }

    void LogFailure(const char* category, const char* phase, const std::string& message)
    {
        const std::string safePhase = SanitizeLogText(
            phase != nullptr ? std::string_view(phase) : std::string_view(),
            "operation");
        Error(category, safePhase + ": " + SanitizeLogText(message, "Unknown error"));
    }
}
