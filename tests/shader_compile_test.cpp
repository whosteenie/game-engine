#include "test_expect.h"

#ifdef _WIN32
#include "engine/rendering/DxrCapabilities.h"
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

    void ExpectShaderLibraryCompiles(
        const char* relativePath,
        const char* message,
        const HlslLibraryCompileOptions& options = {})
    {
        const std::string source = ReadShaderFile(relativePath);
        const HlslCompileResult result = CompileHlslLibrary(source, relativePath, options);
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

        HlslLibraryCompileOptions modernDxrOptions{};
        modernDxrOptions.targetProfile = "lib_6_6";
        modernDxrOptions.defines = {
            "DXR_MODERN_LIBRARY=1",
            "DXR_SER_PERMUTATION=0",
            "DXR_INLINE_VISIBILITY_PERMUTATION=0",
        };
        ExpectShaderLibraryCompiles(
            "assets/shaders/dxr/path_tracer.hlsl",
            "path_tracer.hlsl modern DXR library should compile",
            modernDxrOptions);
        ExpectShaderLibraryCompiles(
            "assets/shaders/dxr/dxr_modern_smoke.hlsl",
            "dxr_modern_smoke.hlsl inline-ray-query library should compile",
            modernDxrOptions);

        const DxrFeatureCapabilities legacyCapabilities{
            static_cast<int>(D3D12_RAYTRACING_TIER_1_0),
            static_cast<int>(D3D_SHADER_MODEL_6_4)};
        test::ExpectTrue(
            !legacyCapabilities.SupportsModernDxrLibrary()
                && !legacyCapabilities.SupportsInlineRaytracing()
                && !legacyCapabilities.SupportsShaderExecutionReordering()
                && std::string(legacyCapabilities.GetPreferredLibraryProfile()) == "lib_6_3",
            "legacy DXR capabilities should choose the clean lib_6_3 fallback");

        const DxrFeatureCapabilities modernCapabilities{
            static_cast<int>(D3D12_RAYTRACING_TIER_1_2),
            static_cast<int>(D3D_SHADER_MODEL_6_6)};
        test::ExpectTrue(
            modernCapabilities.SupportsModernDxrLibrary()
                && modernCapabilities.SupportsInlineRaytracing()
                && modernCapabilities.SupportsShaderExecutionReordering()
                && std::string(modernCapabilities.GetPreferredLibraryProfile()) == "lib_6_6",
            "modern DXR capabilities should enable the lib_6_6 path and report future features");
    }
    catch (const std::exception& exception)
    {
        test::ExpectTrue(false, exception.what());
    }
#endif
}
