#include "test_expect.h"

#ifdef _WIN32
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#endif

namespace
{
#ifdef _WIN32
    std::string ReadShaderFile(const char* relativePath)
    {
        std::ifstream file(relativePath);
        if (!file.is_open())
        {
            throw std::runtime_error(std::string("Failed to open shader file: ") + relativePath);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void ExpectShaderCompiles(
        const char* relativePath,
        const char* entry,
        const char* profile,
        const char* message)
    {
        const std::string source = ReadShaderFile(relativePath);
        const HlslCompileResult result = CompileHlsl(source, relativePath, entry, profile);
        test::ExpectTrue(result.shader != nullptr, message);
    }

    void ExpectShaderLibraryCompiles(const char* relativePath, const char* message)
    {
        const std::string source = ReadShaderFile(relativePath);
        const HlslCompileResult result = CompileHlslLibrary(source, relativePath);
        test::ExpectTrue(result.shader != nullptr, message);
    }
#endif
}

void RunShaderCompileTests()
{
#ifndef _WIN32
    return;
#else
    try
    {
        ExpectShaderCompiles(
            "assets/shaders/fullscreen.vs.hlsl",
            "main",
            "vs_6_0",
            "fullscreen.vs.hlsl should compile");
        ExpectShaderCompiles(
            "assets/shaders/tonemap.ps.hlsl",
            "main",
            "ps_6_0",
            "tonemap.ps.hlsl should compile");
        ExpectShaderCompiles(
            "assets/shaders/pbr.ps.hlsl",
            "main",
            "ps_6_0",
            "pbr.ps.hlsl should compile");
        ExpectShaderCompiles(
            "assets/shaders/scene_gbuffer.as.hlsl",
            "main",
            "as_6_5",
            "scene_gbuffer.as.hlsl (meshlet cull) should compile");
        ExpectShaderCompiles(
            "assets/shaders/scene_gbuffer.ms.hlsl",
            "main",
            "ms_6_5",
            "scene_gbuffer.ms.hlsl should compile");
        ExpectShaderCompiles(
            "assets/shaders/scene_gbuffer_mesh.ps.hlsl",
            "main",
            "ps_6_0",
            "scene_gbuffer_mesh.ps.hlsl (mesh G-buffer lighting) should compile");
        ExpectShaderCompiles(
            "assets/shaders/scene_shadow.as.hlsl",
            "main",
            "as_6_5",
            "scene_shadow.as.hlsl (meshlet cull) should compile");
        ExpectShaderCompiles(
            "assets/shaders/scene_shadow.ms.hlsl",
            "main",
            "ms_6_5",
            "scene_shadow.ms.hlsl should compile");
        ExpectShaderLibraryCompiles(
            "assets/shaders/dxr/path_tracer.hlsl",
            "path_tracer.hlsl DXR library should compile");
        ExpectShaderLibraryCompiles(
            "assets/shaders/dxr/restir_di_temporal.hlsl",
            "restir_di_temporal.hlsl DXR library should compile");
    }
    catch (const std::exception& exception)
    {
        test::ExpectTrue(false, exception.what());
    }
#endif
}
