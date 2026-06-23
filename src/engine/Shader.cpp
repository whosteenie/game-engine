#include <glad/glad.h>

#include "engine/Shader.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <glm/gtc/type_ptr.hpp>

Shader::Shader(const char* vertexPath, const char* fragmentPath)
{
    std::string vertexSource = ReadFile(vertexPath);
    std::string fragmentSource = ReadFile(fragmentPath);

    unsigned int vertex = Compile(GL_VERTEX_SHADER, vertexSource.c_str());
    unsigned int fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource.c_str());
    m_programID = Link(vertex, fragment);
    m_isLinked = m_programID != 0;

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader()
{
    if (m_programID != 0)
    {
        glDeleteProgram(m_programID);
    }
}

void Shader::Use() const
{
    if (!m_isLinked)
    {
        throw std::runtime_error("Attempted to use an unlinked shader program.");
    }

    glUseProgram(m_programID);
}

unsigned int Shader::GetProgramId() const
{
    return m_programID;
}

bool Shader::IsLinked() const
{
    return m_isLinked;
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
    GLint location = glGetUniformLocation(m_programID, name);
    if (location < 0)
    {
        const std::string arrayName = std::string(name) + "[0]";
        location = glGetUniformLocation(m_programID, arrayName.c_str());
    }

    if (location < 0)
    {
        return;
    }

    glUniform1fv(location, count, values);
}

void Shader::SetMat4(const char* name, const glm::mat4& value) const
{
    glUniformMatrix4fv(
        glGetUniformLocation(m_programID, name),
        1,
        GL_FALSE,
        glm::value_ptr(value));
}

void Shader::SetMat4Array(const char* name, const glm::mat4* values, const int count) const
{
    GLint location = glGetUniformLocation(m_programID, name);
    if (location < 0)
    {
        const std::string arrayName = std::string(name) + "[0]";
        location = glGetUniformLocation(m_programID, arrayName.c_str());
    }

    if (location < 0)
    {
        return;
    }

    glUniformMatrix4fv(location, count, GL_FALSE, glm::value_ptr(values[0]));
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

    int success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Shader compile error:\n") + log);
    }

    return shader;
}

unsigned int Shader::Link(unsigned int vertex, unsigned int fragment) const
{
    unsigned int program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    int success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        glDeleteProgram(program);
        throw std::runtime_error(std::string("Shader link error:\n") + log);
    }

    return program;
}
