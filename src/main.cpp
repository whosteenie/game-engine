#include <GLFW/glfw3.h>
#include <iostream>

int main()
{
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Game Engine", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    double lastFrameTime = glfwGetTime();
    
    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        glfwPollEvents();

        // Print FPS once per second
        static double fpsTimer = 0.0;
        fpsTimer += deltaTime;

        if (fpsTimer >= 1.0)
        {
            double fps = 1.0 / deltaTime;
            std::cout << "FPS: " << fps << "  (delta: " << deltaTime << "s)\n";
            fpsTimer = 0.0;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}