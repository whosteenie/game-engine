#include "app/core/Application.h"

#include <iostream>
#include <string>

namespace
{
    void PrintFatalError(const char* prefix, const std::exception& exception)
    {
        std::string details;
        try
        {
            if (const char* what = exception.what(); what != nullptr && what[0] != '\0')
            {
                details = what;
            }
        }
        catch (...)
        {
            details.clear();
        }

        if (details.empty())
        {
            details = typeid(exception).name();
        }

        std::cerr << prefix << details << "\n";
    }
}

int main()
{
    try
    {
        Application app(1280, 720, "Game Engine");
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
