#pragma once

#include <memory>
#include <string>

class Shader;

class ShaderCache
{
public:
    static std::shared_ptr<Shader> Load(const char* vertexPath, const char* fragmentPath);

private:
    ShaderCache() = default;

    static std::string MakeKey(const char* vertexPath, const char* fragmentPath);
};
