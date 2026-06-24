#include "engine/platform/ExceptionMessage.h"

#include "engine/rhi/GfxContext.h"
#include <algorithm>
#include <cctype>
#include <typeinfo>

namespace
{
    bool LooksLikeCorruptMessage(const std::string& message)
    {
        if (message.empty())
        {
            return true;
        }

        if (message.find("Failed") != std::string::npos ||
            message.find("HRESULT") != std::string::npos ||
            message.find("Shader") != std::string::npos ||
            message.find("D3D12") != std::string::npos ||
            message.find("GPU") != std::string::npos)
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

    std::string CopyAsciiWhat(const char* what)
    {
        std::string message;
        if (what == nullptr)
        {
            return message;
        }

        message.reserve(128);
        for (std::size_t index = 0; index < 512 && what[index] != '\0'; ++index)
        {
            const unsigned char byte = static_cast<unsigned char>(what[index]);
            if (byte >= 32 && byte < 127)
            {
                message.push_back(static_cast<char>(byte));
            }
        }

        return message;
    }
}

std::string SafeExceptionMessage(const std::exception& exception)
{
    std::string message;
    try
    {
        message = CopyAsciiWhat(exception.what());
    }
    catch (...)
    {
        message.clear();
    }

    if (LooksLikeCorruptMessage(message))
    {
        message.clear();
    }

    if (message.empty())
    {
        const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        if (!gpuError.empty())
        {
            message = gpuError;
        }
    }
    if (message.empty())
    {
        message = typeid(exception).name();
        message += " (exception message unavailable)";
    }

    return message;
}
