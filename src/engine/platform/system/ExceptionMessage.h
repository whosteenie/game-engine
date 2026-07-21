#pragma once

#include <exception>
#include <string>
#include <string_view>

// Makes arbitrary text safe for stderr, log files, and ImGui (printable ASCII only).
// If the result would be empty or unusable, returns fallbackIfEmpty.
std::string SanitizeLogText(std::string_view text, std::string_view fallbackIfEmpty = "Unknown error");

// Best-effort exception.what() for logging and UI. Never returns an empty string.
std::string SafeExceptionMessageImpl(const char* what, const char* mangledTypeName);

// phase + exception, e.g. "OpenProject: Failed to open file".
std::string FormatExceptionContextImpl(
    const char* context,
    const char* what,
    const char* mangledTypeName);

// Template wrappers avoid MSVC catch-handler stdext::exception vs std::exception link mismatches
// when app code and engine-render.lib are built as separate targets.
template <typename ExceptionType>
std::string SafeExceptionMessage(const ExceptionType& exception)
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

    return SafeExceptionMessageImpl(what, typeid(exception).name());
}

template <typename ExceptionType>
std::string FormatExceptionContext(const char* context, const ExceptionType& exception)
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

    return FormatExceptionContextImpl(context, what, typeid(exception).name());
}
