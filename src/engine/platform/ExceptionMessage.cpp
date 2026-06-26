#include "engine/platform/ExceptionMessage.h"

#include "engine/platform/EngineDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <typeinfo>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    constexpr std::size_t kMaxLogTextLength = 2048;
    constexpr std::size_t kMaxWhatScanLength = 4096;

    std::string DemangleTypeName(const char* mangledName)
    {
        if (mangledName == nullptr || mangledName[0] == '\0')
        {
            return "std::exception";
        }

        std::string typeName = mangledName;
        if (typeName.rfind("class ", 0) == 0)
        {
            typeName.erase(0, 6);
        }
        else if (typeName.rfind("struct ", 0) == 0)
        {
            typeName.erase(0, 7);
        }

        return typeName;
    }

    std::string HexPreview(const unsigned char* bytes, const std::size_t length)
    {
        std::ostringstream stream;
        const std::size_t previewLength = std::min(length, static_cast<std::size_t>(8));
        for (std::size_t index = 0; index < previewLength; ++index)
        {
            if (index > 0)
            {
                stream << ' ';
            }
            stream << std::hex;
            const int value = static_cast<int>(bytes[index]);
            if (value < 16)
            {
                stream << '0';
            }
            stream << value;
        }
        stream << std::dec;
        return stream.str();
    }

    std::string DecodeUtf16LeAsUtf8(const char* what)
    {
#ifdef _WIN32
        if (what == nullptr)
        {
            return {};
        }

        const auto* wide = reinterpret_cast<const wchar_t*>(what);
        std::size_t wideLength = 0;
        for (std::size_t index = 0; index < 2048 && wide[index] != L'\0'; ++index)
        {
            ++wideLength;
        }

        if (wideLength == 0)
        {
            return {};
        }

        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide,
            static_cast<int>(wideLength),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0)
        {
            return {};
        }

        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            wide,
            static_cast<int>(wideLength),
            utf8.data(),
            utf8Length,
            nullptr,
            nullptr);
        return utf8;
#else
        (void)what;
        return {};
#endif
    }

    std::string CopyExceptionWhat(const char* what, bool* outHadNonAscii)
    {
        std::string message;
        if (what == nullptr)
        {
            if (outHadNonAscii != nullptr)
            {
                *outHadNonAscii = false;
            }
            return message;
        }

        message.reserve(128);
        bool hadNonAscii = false;
        for (std::size_t index = 0; index < kMaxWhatScanLength && what[index] != '\0'; ++index)
        {
            const unsigned char byte = static_cast<unsigned char>(what[index]);
            if (byte == '\n' || byte == '\r' || byte == '\t')
            {
                message.push_back(' ');
                continue;
            }

            if (byte >= 32 && byte < 127)
            {
                message.push_back(static_cast<char>(byte));
            }
            else
            {
                hadNonAscii = true;
            }
        }

        if (outHadNonAscii != nullptr)
        {
            *outHadNonAscii = hadNonAscii;
        }

        while (!message.empty() && message.back() == ' ')
        {
            message.pop_back();
        }

        return message;
    }

    bool LooksLikeCorruptMessage(const std::string& message)
    {
        if (message.empty())
        {
            return true;
        }

        if (message.find("Failed") != std::string::npos ||
            message.find("failed") != std::string::npos ||
            message.find("error") != std::string::npos ||
            message.find("Error") != std::string::npos ||
            message.find("HRESULT") != std::string::npos ||
            message.find("Shader") != std::string::npos ||
            message.find("D3D12") != std::string::npos ||
            message.find("GPU") != std::string::npos ||
            message.find("json") != std::string::npos ||
            message.find("project") != std::string::npos ||
            message.find("Project") != std::string::npos)
        {
            return false;
        }

        const std::size_t questionCount = static_cast<std::size_t>(
            std::count(message.begin(), message.end(), '?'));
        if (questionCount >= 8)
        {
            return true;
        }

        int letterCount = 0;
        for (const char character : message)
        {
            if (std::isalpha(static_cast<unsigned char>(character)) != 0)
            {
                ++letterCount;
            }
        }

        return letterCount < 4;
    }
}

std::string SanitizeLogText(const std::string_view text, const std::string_view fallbackIfEmpty)
{
    std::string sanitized;
    sanitized.reserve(std::min(text.size(), kMaxLogTextLength));

    int letterCount = 0;
    for (std::size_t index = 0; index < text.size() && sanitized.size() < kMaxLogTextLength; ++index)
    {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte == '\n' || byte == '\r' || byte == '\t')
        {
            sanitized.push_back(' ');
            continue;
        }

        if (byte >= 32 && byte < 127)
        {
            sanitized.push_back(static_cast<char>(byte));
            if (std::isalpha(byte) != 0)
            {
                ++letterCount;
            }
        }
    }

    while (!sanitized.empty() && sanitized.back() == ' ')
    {
        sanitized.pop_back();
    }

    if (sanitized.empty() || letterCount < 2)
    {
        if (!text.empty())
        {
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text.data());
            const std::size_t byteLength = std::min(text.size(), static_cast<std::size_t>(64));
            return std::string(fallbackIfEmpty)
                + " (non-text payload, " + std::to_string(text.size()) + " bytes, hex "
                + HexPreview(bytes, byteLength) + ")";
        }

        return std::string(fallbackIfEmpty);
    }

    if (sanitized.size() < text.size())
    {
        sanitized += " (truncated)";
    }

    return sanitized;
}

std::string SafeExceptionMessage(const std::exception& exception)
{
    bool hadNonAscii = false;
    std::string message;
    try
    {
        message = CopyExceptionWhat(exception.what(), &hadNonAscii);
        if (message.empty() && hadNonAscii)
        {
            message = DecodeUtf16LeAsUtf8(exception.what());
            if (!message.empty())
            {
                hadNonAscii = false;
            }
        }
    }
    catch (...)
    {
        message.clear();
        hadNonAscii = true;
    }

    const bool rejectedAsCorrupt = LooksLikeCorruptMessage(message);
    if (rejectedAsCorrupt)
    {
        message.clear();
    }

    if (message.empty())
    {
        const std::string gpuError = EngineDiagnostics::GetLastGpuAllocationError();
        if (!gpuError.empty())
        {
            message = SanitizeLogText(gpuError, "GPU allocation failed");
        }
    }

    if (message.empty())
    {
        const std::string typeName = DemangleTypeName(typeid(exception).name());
        message = typeName;
        if (hadNonAscii)
        {
            try
            {
                const char* rawWhat = exception.what();
                if (rawWhat != nullptr)
                {
                    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(rawWhat);
                    std::size_t byteLength = 0;
                    for (; byteLength < 64 && rawWhat[byteLength] != '\0'; ++byteLength)
                    {
                    }

                    if (byteLength > 0)
                    {
                        message += " (exception message contained non-ASCII or unreadable data, hex "
                            + HexPreview(bytes, byteLength) + ")";
                    }
                    else
                    {
                        message += " (exception message contained non-ASCII or unreadable data)";
                    }
                }
                else
                {
                    message += " (exception message contained non-ASCII or unreadable data)";
                }
            }
            catch (...)
            {
                message += " (exception message contained non-ASCII or unreadable data)";
            }
        }
        else if (rejectedAsCorrupt)
        {
            message += " (exception message was empty or unusable)";
        }
        else
        {
            message += " (exception message unavailable)";
        }
    }

    return message;
}

std::string FormatExceptionContext(const char* context, const std::exception& exception)
{
    const std::string contextText = SanitizeLogText(
        context != nullptr ? std::string_view(context) : std::string_view(),
        "operation");
    return contextText + ": " + SafeExceptionMessage(exception);
}
