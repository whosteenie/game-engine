#include "test_expect.h"

#ifdef _WIN32
#include "engine/rendering/core/DxrCapabilities.h"
#include "engine/rendering/core/DxrRuntimeSnapshot.h"
#include "engine/raytracing/dispatch/DxrPathTracerDispatch.h"
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
        ExpectShaderCompiles(
            "assets/shaders/post/utility/rr_temporal_validity.ps.hlsl",
            "main", "ps_6_0", "RR temporal-validity shader should compile");
        ExpectShaderCompiles(
            "assets/shaders/post/utility/rr_history_copy.ps.hlsl",
            "main", "ps_6_0", "RR history-copy shader should compile");
        ExpectShaderCompiles(
            "assets/shaders/post/bloom/bloom_temporal.ps.hlsl",
            "main", "ps_6_0", "bloom temporal shader should compile");
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl DXR library should compile");
        HlslLibraryCompileOptions pathTracerRayGenerationOptions{};
        pathTracerRayGenerationOptions.exports = {"PathTracerShadeRayGen"};
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl partitioned shading ray-generation library should compile",
            pathTracerRayGenerationOptions);
        HlslLibraryCompileOptions pathTracerPsrResolveOptions{};
        pathTracerPsrResolveOptions.exports = {"PathTracerPsrResolveRayGen"};
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl partitioned PSR resolve library should compile",
            pathTracerPsrResolveOptions);
        HlslLibraryCompileOptions pathTracerTraceOptions{};
        pathTracerTraceOptions.exports = {"PathTracerMiss", "PathTracerClosestHit"};
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl partitioned trace library should compile",
            pathTracerTraceOptions);
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/restir_di_temporal.hlsl",
            "restir_di_temporal.hlsl DXR library should compile");

        HlslLibraryCompileOptions modernDxrOptions{};
        modernDxrOptions.targetProfile = "lib_6_6";
        modernDxrOptions.defines = {
            "DXR_MODERN_LIBRARY=1",
            "DXR_SER_PERMUTATION=0",
            "DXR_INLINE_VISIBILITY_PERMUTATION=0",
        };
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl modern DXR library should compile",
            modernDxrOptions);

        HlslLibraryCompileOptions diagnosticDxrOptions = modernDxrOptions;
        diagnosticDxrOptions.defines.push_back("PT_DIAGNOSTIC_PERMUTATION=1");
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl diagnostic DXR library should compile",
            diagnosticDxrOptions);

        HlslLibraryCompileOptions inlineVisibilityOptions = modernDxrOptions;
        inlineVisibilityOptions.defines[2] = "DXR_INLINE_VISIBILITY_PERMUTATION=1";
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl inline-visibility DXR library should compile",
            inlineVisibilityOptions);

        HlslLibraryCompileOptions inlineVisibility65Options = inlineVisibilityOptions;
        inlineVisibility65Options.targetProfile = "lib_6_5";
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl SM 6.5 inline-visibility library should compile",
            inlineVisibility65Options);
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/libraries/dxr_modern_smoke.hlsl",
            "dxr_modern_smoke.hlsl inline-ray-query library should compile",
            modernDxrOptions);

        HlslLibraryCompileOptions serDxrOptions{};
        serDxrOptions.targetProfile = "lib_6_9";
        serDxrOptions.defines = {
            "DXR_MODERN_LIBRARY=1",
            "DXR_SER_PERMUTATION=1",
            "DXR_INLINE_VISIBILITY_PERMUTATION=1",
        };
        ExpectShaderLibraryCompiles(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl",
            "path_tracer.hlsl SER DXR library should compile",
            serDxrOptions);

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
                && !modernCapabilities.SupportsShaderExecutionReordering()
                && std::string(modernCapabilities.GetPreferredLibraryProfile()) == "lib_6_6",
            "SM 6.6 DXR capabilities should retain the lib_6_6 non-SER path");

        const DxrFeatureCapabilities serCapabilities{
            static_cast<int>(D3D12_RAYTRACING_TIER_1_2),
            static_cast<int>(D3D_SHADER_MODEL_6_9)};
        test::ExpectTrue(
            serCapabilities.SupportsShaderExecutionReordering()
                && std::string(serCapabilities.GetPreferredLibraryProfile()) == "lib_6_6",
            "SER selection should be fixed by SM 6.9 capability");

        const DxrFeatureCapabilities inlineCapabilities{
            static_cast<int>(D3D12_RAYTRACING_TIER_1_1),
            static_cast<int>(D3D_SHADER_MODEL_6_5)};
        test::ExpectTrue(
            inlineCapabilities.SupportsInlineRaytracing()
                && !inlineCapabilities.SupportsModernDxrLibrary()
                && std::string(inlineCapabilities.GetPreferredLibraryProfile()) == "lib_6_5",
            "SM 6.5 inline DXR capabilities should select the lib_6_5 permutation");

        DxrRuntimeSnapshot missingSnapshot{};
        const std::string missingJson = SerializeDxrRuntimeSnapshotJson(missingSnapshot);
        test::ExpectTrue(
            missingJson == SerializeDxrRuntimeSnapshotJson(missingSnapshot)
                && missingJson.find("\"schema_version\":1") != std::string::npos
                && missingJson.find("\"query\":\"not_attempted\"") != std::string::npos,
            "DXR runtime snapshot serialization should be stable and explicit for missing queries");

        DxrRuntimeSnapshot unsupportedSnapshot{};
        unsupportedSnapshot.options22Query = "unsupported_or_query_failed";
        unsupportedSnapshot.selectedPermutation = "fallback_production";
        unsupportedSnapshot.fallbackReason = "capability_gate_not_supported";
        const std::string unsupportedJson = SerializeDxrRuntimeSnapshotJson(unsupportedSnapshot);
        test::ExpectTrue(
            unsupportedJson.find("\"query\":\"unsupported_or_query_failed\"") != std::string::npos
                && unsupportedJson.find("\"fallback_reason\":\"capability_gate_not_supported\"")
                    != std::string::npos,
            "DXR runtime snapshot should distinguish unsupported capability fallback");

        DxrRuntimeSnapshot supportedSnapshot{};
        supportedSnapshot.options22Query = "succeeded";
        supportedSnapshot.options22ActuallyReorders = true;
        supportedSnapshot.permutations[DxrRuntimeSerProduction] = {"succeeded", "succeeded"};
        supportedSnapshot.selectedPermutation = "ser_production";
        supportedSnapshot.dispatchedPermutation = "ser_production";
        supportedSnapshot.fallbackReason = "none";
        const std::string supportedJson = SerializeDxrRuntimeSnapshotJson(supportedSnapshot);
        test::ExpectTrue(
            supportedJson.find("\"shader_execution_reordering_actually_reorders\":true")
                    != std::string::npos
                && supportedJson.find(
                    "\"ser_production\":{\"compiler_library\":\"succeeded\",\"rtpso\":\"succeeded\"}")
                    != std::string::npos
                && supportedJson.find("\"dispatched_permutation\":\"ser_production\"")
                    != std::string::npos,
            "DXR runtime snapshot should report successful SER library, RTPSO, and dispatch state");
    }
    catch (const std::exception& exception)
    {
        test::ExpectTrue(false, exception.what());
    }
#endif
}
