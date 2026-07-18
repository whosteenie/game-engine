#include "app/core/Application.h"

#include "engine/platform/system/CrashHandler.h"
#include "engine/platform/system/ExceptionMessage.h"

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
    try
    {
        Application app(1280, 720, "Who Engine");
        app.Run();
    }
    catch (const std::exception& e)
    {
        PrintFatalError("Fatal error: ", e);
        return 1;
    }
    catch (...)
    {
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }

    return 0;
}
