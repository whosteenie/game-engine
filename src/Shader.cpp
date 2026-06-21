#include <glad/glad.h>

#include "Shader.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>

Shader::Shader(const char* vertexPath, const char* fragmentPath)
{
    std::string vertexSource = ReadFile(vertexPath);
    std::string fragmentSource = ReadFile(fragmentPath);

    unsigned int vertex = Compile(GL_VERTEX_SHADER, vertexSource.c_str());
    unsigned int fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource.c_str());
    m_programID = Link(vertex, fragment);

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader()
{
    glDeleteProgram(m_programID);
}

void Shader::Use() const
{
    glUseProgram(m_programID);
}

void Shader::SetFloat(const char* name, float value) const
{
    glUniform1f(glGetUniformLocation(m_programID, name), value);
}

std::string Shader::ReadFile(const char* filepath) const
{
    std::ifstream file(filepath);
    if (!file.is_open())
    {
        std::cerr << "Failed to open shader file: " << filepath << "\n";
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

unsigned int Shader::Compile(unsigned int type, const char* source) const
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

unsigned int Shader::Link(unsigned int vertex, unsigned int fragment) const
{
    unsigned int program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        std::cerr << "Shader link error:\n" << log << "\n";
    }

    return program;
}

void Shader::SetMat4(const char* name, const glm::mat4& value) const
{
    glUniformMatrix4fv(
        glGetUniformLocation(m_programID, name),
        1,
        GL_FALSE,
        glm::value_ptr(value)
    );
}
