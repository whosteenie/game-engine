#include <glad/glad.h>

#include "engine/rendering/Shader.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <glm/gtc/type_ptr.hpp>

namespace
{
    class ShaderObjectGuard
    {
    public:
        explicit ShaderObjectGuard(unsigned int shaderId = 0)
            : m_shaderId(shaderId)
        {
        }

        ~ShaderObjectGuard()
        {
            if (m_shaderId != 0)
            {
                glDeleteShader(m_shaderId);
            }
        }

        ShaderObjectGuard(const ShaderObjectGuard&) = delete;
        ShaderObjectGuard& operator=(const ShaderObjectGuard&) = delete;

        ShaderObjectGuard(ShaderObjectGuard&& other) noexcept
            : m_shaderId(other.m_shaderId)
        {
            other.m_shaderId = 0;
        }

        ShaderObjectGuard& operator=(ShaderObjectGuard&& other) noexcept
        {
            if (this != &other)
            {
                if (m_shaderId != 0)
                {
                    glDeleteShader(m_shaderId);
                }

                m_shaderId = other.m_shaderId;
                other.m_shaderId = 0;
            }

            return *this;
        }

        unsigned int Get() const
        {
            return m_shaderId;
        }

        unsigned int Release()
        {
            const unsigned int released = m_shaderId;
            m_shaderId = 0;
            return released;
        }

    private:
        unsigned int m_shaderId = 0;
    };
}

Shader::Shader(const char* vertexPath, const char* fragmentPath)
{
    const std::string vertexSource = ReadFile(vertexPath);
    const std::string fragmentSource = ReadFile(fragmentPath);

    ShaderObjectGuard vertex(Compile(GL_VERTEX_SHADER, vertexSource.c_str()));
    ShaderObjectGuard fragment(Compile(GL_FRAGMENT_SHADER, fragmentSource.c_str()));
    m_programID = Link(vertex.Get(), fragment.Get());
    m_isLinked = m_programID != 0;

    glDeleteShader(vertex.Release());
    glDeleteShader(fragment.Release());
}

Shader::~Shader()
{
    if (m_programID != 0)
    {
        glDeleteProgram(m_programID);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_programID(other.m_programID),
      m_isLinked(other.m_isLinked)
{
    other.m_programID = 0;
    other.m_isLinked = false;
}

Shader& Shader::operator=(Shader&& other) noexcept
{
    if (this != &other)
    {
        if (m_programID != 0)
        {
            glDeleteProgram(m_programID);
        }

        m_programID = other.m_programID;
        m_isLinked = other.m_isLinked;
        other.m_programID = 0;
        other.m_isLinked = false;
    }

    return *this;
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

bool Shader::HasUniform(const char* name) const
{
    return glGetUniformLocation(m_programID, name) >= 0;
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
