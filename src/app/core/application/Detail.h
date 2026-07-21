#pragma once

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/platform/system/ExceptionMessage.h"

#include <exception>
#include <stdexcept>
#include <string>

struct GLFWwindow;

namespace ApplicationDetail
{
    bool ConsumeConsoleCloseRequest();
    void DetachStartupWindowPaint(GLFWwindow* window);

    template<typename Fn>
    void RunPhase(const char* phase, Fn&& fn)
    {
        try
        {
            fn();
        }
        catch (const std::exception& exception)
        {
            SceneRenderTrace::Step(std::string("exception in ") + phase);
            const std::string safeMessage = SafeExceptionMessage(exception);
            EngineLog::LogFailure("application", phase, safeMessage);
            throw std::runtime_error(std::string(phase) + ": " + safeMessage);
        }
        catch (...)
        {
            SceneRenderTrace::Step(std::string("non-std exception in ") + phase);
            EngineLog::Error("application", std::string(phase) + ": non-standard exception");
            throw std::runtime_error(std::string(phase) + ": non-standard exception");
        }
    }
}
