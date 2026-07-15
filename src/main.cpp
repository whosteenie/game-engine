#include "app/core/Application.h"

#include "engine/platform/CrashHandler.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/ProjectLoadBenchmark.h"

#include <iostream>
#include <string>

namespace
{
    void PrintFatalError(const char* prefix, const std::exception& exception)
    {
        std::cerr << prefix << SafeExceptionMessage(exception) << "\n";
    }
}

int main()
{
    // Install first so hard faults (access violations, GPU driver faults) during startup/load are
    // captured with a symbolized stack even when they never unwind the C++ stack.
    CrashHandler::Install();
    ProjectLoadBenchmark::StartFromEnvironment();
    ProjectLoadBenchmark::Mark("process.main.begin");

    try
    {
        ProjectLoadBenchmark::Mark("application.construct.begin");
        Application app(1280, 720, "Who Engine");
        ProjectLoadBenchmark::Mark("application.construct.complete");
        app.Run();
    }
    catch (const std::exception& e)
    {
        ProjectLoadBenchmark::Fail(SafeExceptionMessage(e));
        PrintFatalError("Fatal error: ", e);
        return 1;
    }
    catch (...)
    {
        ProjectLoadBenchmark::Fail("Fatal error: unknown exception");
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }

    return 0;
}
