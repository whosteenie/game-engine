#include "engine/rendering/Shader.h"

#include <glm/gtc/type_ptr.hpp>

Shader::Shader(const char* /*vertexPath*/, const char* /*fragmentPath*/)
{
}

Shader::~Shader() = default;

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
        m_programID = other.m_programID;
        m_isLinked = other.m_isLinked;
        other.m_programID = 0;
        other.m_isLinked = false;
    }

    return *this;
}

void Shader::Use(const bool /*mrtPass*/) const
{
}

void Shader::SetFloat(const char* /*name*/, float /*value*/) const
{
}

void Shader::SetInt(const char* /*name*/, int /*value*/) const
{
}

void Shader::SetIntArray(const char* /*name*/, const int* /*values*/, int /*count*/) const
{
}

void Shader::SetFloatArray(const char* /*name*/, const float* /*values*/, int /*count*/) const
{
}

void Shader::SetMat4(const char* /*name*/, const glm::mat4& /*value*/) const
{
}

void Shader::SetMat4Array(const char* /*name*/, const glm::mat4* /*values*/, int /*count*/) const
{
}

void Shader::SetVec2(const char* /*name*/, const glm::vec2& /*value*/) const
{
}

void Shader::SetVec3(const char* /*name*/, const glm::vec3& /*value*/) const
{
}

void Shader::SetVec3Array(const char* /*name*/, const glm::vec3* /*values*/, int /*count*/) const
{
}

unsigned int Shader::GetProgramId() const
{
    return m_programID;
}

bool Shader::IsLinked() const
{
    return m_isLinked;
}

bool Shader::HasUniform(const char* /*name*/) const
{
    return false;
}
