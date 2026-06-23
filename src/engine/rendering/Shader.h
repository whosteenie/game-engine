#pragma once

#include <glm/glm.hpp>
#include <string>

class Shader
{
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    void Use() const;
    void SetFloat(const char* name, float value) const;
    void SetInt(const char* name, int value) const;
    void SetIntArray(const char* name, const int* values, int count) const;
    void SetFloatArray(const char* name, const float* values, int count) const;
    void SetMat4(const char* name, const glm::mat4& value) const;
    void SetMat4Array(const char* name, const glm::mat4* values, int count) const;
    void SetVec2(const char* name, const glm::vec2& value) const;
    void SetVec3(const char* name, const glm::vec3& value) const;
    void SetVec3Array(const char* name, const glm::vec3* values, int count) const;

    unsigned int GetProgramId() const;
    bool IsLinked() const;

private:
    unsigned int m_programID = 0;
    bool m_isLinked = false;

    std::string ReadFile(const char* filepath) const;
    unsigned int Compile(unsigned int type, const char* source) const;
    unsigned int Link(unsigned int vertex, unsigned int fragment) const;
};
