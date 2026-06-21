#pragma once

#include <string>

class Shader
{
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    void Use() const;
    void SetFloat(const char* name, float value) const;

private:
    unsigned int m_programID;

    std::string ReadFile(const char* filepath) const;
    unsigned int Compile(unsigned int type, const char* source) const;
    unsigned int Link(unsigned int vertex, unsigned int fragment) const;
};
