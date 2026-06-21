#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <iostream>

const char* vertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    uniform float uTime;
    uniform float uAspect;
    void main()
    {
        float angle = uTime * 1.5;
        float c = cos(angle);
        float s = sin(angle);
        // Rotate around Y axis
        vec3 rotated;
        rotated.x = aPos.x * c + aPos.z * s;
        rotated.y = aPos.y;
        rotated.z = -aPos.x * s + aPos.z * c;
        gl_Position = vec4(rotated.x * uAspect, rotated.y, rotated.z, 1.0);
    }
    )";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;

uniform float uTime;

void main()
{
    FragColor = vec4(
        0.5 + 0.5 * sin(uTime),
        0.5 + 0.5 * sin(uTime + 2.094),
        0.5 + 0.5 * sin(uTime + 4.189),
        1.0
    );
}
)";

unsigned int CompileShader(unsigned int type, const char* source)
{
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "Shader compile error:\n" << log << "\n";
    }
    return shader;
}

unsigned int CreateShaderProgram(const char* vertexSrc, const char* fragmentSrc)
{
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    unsigned int program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        std::cerr << "Shader link error:\n" << log << "\n";
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

int main()
{
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Game Engine", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD\n";
        return 1;
    }
    
    glfwSwapInterval(1);

    glEnable(GL_DEPTH_TEST);

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    float aspect = static_cast<float>(framebufferHeight) / static_cast<float>(framebufferWidth);

    unsigned int shaderProgram = CreateShaderProgram(vertexShaderSource, fragmentShaderSource);
    int uTimeLocation = glGetUniformLocation(shaderProgram, "uTime");
    int uAspectLocation = glGetUniformLocation(shaderProgram, "uAspect");

    // Equilateral triangle centered at origin (circumradius 0.5, point facing up)
    const float k = 0.4330127f; // sqrt(3) / 4 * 2 = sqrt(3)/2 * 0.5
    float vertices[] = {
         0.0f,  0.5f, 0.0f,  // top
        -k,    -0.25f, 0.0f, // bottom-left
         k,    -0.25f, 0.0f  // bottom-right
    };
    unsigned int vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    double lastFrameTime = glfwGetTime();
    
    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        glfwPollEvents();

        glClearColor(0.08f, 0.09f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(shaderProgram);
        glUniform1f(uAspectLocation, aspect);
        glUniform1f(uTimeLocation, static_cast<float>(currentTime));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glfwSwapBuffers(window);

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