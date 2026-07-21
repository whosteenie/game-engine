#pragma once

#include <string>

namespace ProjectLoadTrace
{
    // Reset the step counter at the start of a project open attempt.
    void Reset();

    // Always logged to stderr and diagnostics/engine.log (not gated by GAME_ENGINE_LOG).
    void Step(const char* message);
    void Step(const std::string& message);

    // Logs step on construction; logs "<step> ok" on Success(); logs "<step> FAILED" on
    // destruction if Success() was not called (e.g. exception unwinding).
    class Scope
    {
    public:
        explicit Scope(const char* step);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        void Success();

    private:
        const char* m_step = nullptr;
        bool m_success = false;
    };
}
