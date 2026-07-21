#pragma once

#include <string>

namespace SceneRenderTrace
{
    // Reset when a project open completes and the first scene render is pending.
    void Reset();

    // Call after the first scene-view render frame (success or failure).
    void CompleteFirstFrame();

    bool IsActive();

    // Ensures CompleteFirstFrame() runs when leaving scene-view-render.
    class FirstFrameGuard
    {
    public:
        FirstFrameGuard() = default;
        ~FirstFrameGuard() { CompleteFirstFrame(); }

        FirstFrameGuard(const FirstFrameGuard&) = delete;
        FirstFrameGuard& operator=(const FirstFrameGuard&) = delete;
    };

    // Always logged to stderr and diagnostics/engine.log while IsActive() (not gated by GAME_ENGINE_LOG).
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
        bool m_active = false;
    };

    // Feature section: logs "--- <name> begin ---", then "--- <name> ok ---" or FAILED.
    class Section
    {
    public:
        explicit Section(const char* sectionName);
        ~Section();

        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;

        void Success();

    private:
        const char* m_section = nullptr;
        bool m_success = false;
        bool m_active = false;
    };
}
