#pragma once

namespace CrashHandler
{
    // Installs a process-wide unhandled-exception filter (Windows only) that writes a symbolized
    // crash report (exception code, faulting address, and a resolved call stack) to the engine log
    // before the process dies. This is the tool of last resort for hard faults (access violations,
    // GPU driver faults) that never unwind the C++ stack, so try/catch and breadcrumbs see nothing.
    // No-op on non-Windows builds. Safe to call once, early in main().
    void Install();

    // Optional hook run from inside the crash filter, right before the stack is written. Used to
    // flush subsystem state that only exists at crash time — e.g. the D3D12 debug-layer info queue,
    // which holds the exact validation message behind driver/debug-layer faults. Must be crash-safe
    // (no locks that could already be held). Pass nullptr to clear.
    using ContextHook = void (*)();
    void SetContextHook(ContextHook hook);
}
