#include "engine/platform/SceneRenderTrace.h"

#include "engine/platform/EngineLog.h"

#include <cstdio>
#include <string>

namespace
{
    int g_stepCounter = 0;
    bool g_active = false;

    std::string FormatStepMessage(const std::string& message)
    {
        char prefix[8]{};
        std::snprintf(prefix, sizeof(prefix), "%03d", ++g_stepCounter);
        return std::string(prefix) + " " + message;
    }

    void LogStep(const char* message)
    {
        if (!g_active || message == nullptr)
        {
            return;
        }

        EngineLog::Breadcrumb("render", FormatStepMessage(message));
    }

    void LogStep(const std::string& message)
    {
        if (!g_active)
        {
            return;
        }

        EngineLog::Breadcrumb("render", FormatStepMessage(message));
    }
}

namespace SceneRenderTrace
{
    void Reset()
    {
        g_stepCounter = 0;
        g_active = true;
        EngineLog::Breadcrumb("render", "=== first render trace begin ===");
    }

    void CompleteFirstFrame()
    {
        if (!g_active)
        {
            return;
        }

        LogStep("=== first render trace complete ===");
        g_active = false;
    }

    bool IsActive()
    {
        return g_active;
    }

    void Step(const char* message)
    {
        LogStep(message);
    }

    void Step(const std::string& message)
    {
        LogStep(message);
    }

    Scope::Scope(const char* step) : m_step(step), m_active(g_active)
    {
        LogStep(step);
    }

    Scope::~Scope()
    {
        if (!m_active || m_success || m_step == nullptr)
        {
            return;
        }

        LogStep(std::string(m_step) + " FAILED");
    }

    void Scope::Success()
    {
        if (!m_active || m_success || m_step == nullptr)
        {
            return;
        }

        LogStep(std::string(m_step) + " ok");
        m_success = true;
    }

    Section::Section(const char* sectionName) : m_section(sectionName), m_active(g_active)
    {
        if (!m_active || sectionName == nullptr)
        {
            m_active = false;
            return;
        }

        LogStep(std::string("--- ") + sectionName + " begin ---");
    }

    Section::~Section()
    {
        if (!m_active || m_success || m_section == nullptr)
        {
            return;
        }

        LogStep(std::string("--- ") + m_section + " FAILED ---");
    }

    void Section::Success()
    {
        if (!m_active || m_success || m_section == nullptr)
        {
            return;
        }

        LogStep(std::string("--- ") + m_section + " ok ---");
        m_success = true;
    }
}
