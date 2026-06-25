#pragma once

#include <exception>
#include <string>
#include <string_view>

// Makes arbitrary text safe for stderr, log files, and ImGui (printable ASCII only).
// If the result would be empty or unusable, returns fallbackIfEmpty.
std::string SanitizeLogText(std::string_view text, std::string_view fallbackIfEmpty = "Unknown error");

// Best-effort exception.what() for logging and UI. Never returns an empty string.
std::string SafeExceptionMessage(const std::exception& exception);

// phase + exception, e.g. "OpenProject: Failed to open file".
std::string FormatExceptionContext(const char* context, const std::exception& exception);
