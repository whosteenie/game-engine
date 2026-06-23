#include "app/core/Application.h"

#include <iostream>

int main()
{
    try
    {
        Application app(1280, 720, "Game Engine");
        app.Run();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
