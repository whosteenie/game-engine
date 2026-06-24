#pragma once

#include <string>

namespace ShaderPath
{
    std::string ToHlslPath(const char* glslPath);
    bool IsVertexShader(const std::string& hlslPath);
}
