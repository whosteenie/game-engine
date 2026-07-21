#include "engine/platform/diagnostics/ProjectLoadTrace.h"

#include "engine/platform/diagnostics/EngineLog.h"

#include <cstdio>
#include <string>

namespace
{
    int g_stepCounter = 0;

    std::string FormatStepMessage(const std::string& message)
    {
        char prefix[8]{};
        std::snprintf(prefix, sizeof(prefix), "%03d", ++g_stepCounter);
        return std::string(prefix) + " " + message;
    }
}

namespace ProjectLoadTrace
{
    void Reset()
    {
        g_stepCounter = 0;
        EngineLog::Breadcrumb("load", "=== project load begin ===");
    }

    void Step(const char* message)
    {
        if (message == nullptr)
        {
            EngineLog::Breadcrumb("load", FormatStepMessage("(null step)"));
            return;
        }

        EngineLog::Breadcrumb("load", FormatStepMessage(message));
    }

    void Step(const std::string& message)
    {
        EngineLog::Breadcrumb("load", FormatStepMessage(message));
    }

    Scope::Scope(const char* step) : m_step(step)
    {
        Step(step);
    }

    Scope::~Scope()
    {
        if (m_success || m_step == nullptr)
        {
            return;
        }

        EngineLog::Breadcrumb("load", FormatStepMessage(std::string(m_step) + " FAILED"));
    }

    void Scope::Success()
    {
        if (m_success || m_step == nullptr)
        {
            return;
        }

        EngineLog::Breadcrumb("load", FormatStepMessage(std::string(m_step) + " ok"));
        m_success = true;
    }
}
