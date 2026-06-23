#include <glad/glad.h>

#include "engine/Shader.h"

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

void Shader::SetInt(const char* name, int value) const
{
    glUniform1i(glGetUniformLocation(m_programID, name), value);
}

void Shader::SetIntArray(const char* name, const int* values, int count) const
{
    glUniform1iv(glGetUniformLocation(m_programID, name), count, values);
}

void Shader::SetFloatArray(const char* name, const float* values, int count) const
{
    glUniform1fv(glGetUniformLocation(m_programID, name), count, values);
}

void Shader::SetMat4(const char* name, const glm::mat4& value) const
{
    glUniformMatrix4fv(
        glGetUniformLocation(m_programID, name),
        1,
        GL_FALSE,
        glm::value_ptr(value));
}

void Shader::SetVec2(const char* name, const glm::vec2& value) const
{
    glUniform2fv(glGetUniformLocation(m_programID, name), 1, glm::value_ptr(value));
}

void Shader::SetVec3(const char* name, const glm::vec3& value) const
{
    glUniform3fv(glGetUniformLocation(m_programID, name), 1, glm::value_ptr(value));
}

void Shader::SetVec3Array(const char* name, const glm::vec3* values, int count) const
{
    glUniform3fv(glGetUniformLocation(m_programID, name), count, glm::value_ptr(values[0]));
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
