#include "engine/rhi/d3d12/ShaderPath.h"

#include <filesystem>

namespace
{
    bool EndsWith(const std::string& value, const char* suffix)
    {
        const std::string suffixString(suffix);
        if (value.size() < suffixString.size())
        {
            return false;
        }

        return value.compare(value.size() - suffixString.size(), suffixString.size(), suffixString) == 0;
    }

    std::string ReplaceExtension(const std::string& path, const char* newExt)
    {
        std::filesystem::path filePath(path);
        return (filePath.parent_path() / (filePath.stem().string() + newExt)).string();
    }
}

namespace ShaderPath
{
    std::string ToHlslPath(const char* glslPath)
    {
        const std::string path(glslPath);
        if (EndsWith(path, ".vert"))
        {
            return ReplaceExtension(path, ".vs.hlsl");
        }

        if (EndsWith(path, ".frag"))
        {
            return ReplaceExtension(path, ".ps.hlsl");
        }

        return path;
    }

    bool IsVertexShader(const std::string& hlslPath)
    {
        return EndsWith(hlslPath, ".vs.hlsl");
    }
}
