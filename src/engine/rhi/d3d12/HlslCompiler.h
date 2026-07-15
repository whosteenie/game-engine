#pragma once

#include <d3d12shader.h>
#include <dxcapi.h>

#include <string>
#include <vector>

#include <wrl/client.h>

struct HlslCompileResult
{
    Microsoft::WRL::ComPtr<IDxcBlob> shader;
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
};

// DXC lib_* output is a DXBC container; D3D12 DXIL_LIBRARY needs the DXIL part only.
struct DxilLibraryBytecode
{
    Microsoft::WRL::ComPtr<IDxcBlob> bytecode;
    bool extractedFromDxbcContainer = false;
    std::size_t containerByteCount = 0;
};

struct HlslStageCompileRequest
{
    const char* sourcePath = nullptr;
    const char* entry = "main";
    const char* targetProfile = nullptr;
};

// The target profile and defines are part of the compiled DXIL contract. Keeping them explicit
// makes a modern DXR library a deliberate, cacheable permutation rather than an ambient toolchain
// setting.
struct HlslLibraryCompileOptions
{
    const char* targetProfile = "lib_6_3";
    std::vector<std::string> defines;
};

DxilLibraryBytecode PrepareDxilLibraryBytecode(Microsoft::WRL::ComPtr<IDxcBlob> dxcOutput);

HlslCompileResult CompileHlsl(
    const std::string& source,
    const std::string& sourcePath,
    const char* entry,
    const char* targetProfile);

// Raster programs commonly share an immutable stage (for example fullscreen.vs.hlsl).
// Retain the compiler outputs separately from Shader objects so reconstructing a program
// can still rebuild its root signature/PSOs without repeatedly invoking DXC for that stage.
// Call this alongside the higher-level pipeline caches when explicitly invalidating shaders.
void ClearHlslStageCompileCache();

// Compile independent raster stages concurrently into the same immutable stage cache used by
// CompileHlsl. This deliberately prewarms bytecode/reflection only; callers remain responsible
// for creating their normal root signatures and PSOs on the owning render thread.
void PrewarmHlslStages(const std::vector<HlslStageCompileRequest>& requests);

HlslCompileResult CompileHlslLibrary(
    const std::string& source,
    const std::string& sourcePath,
    const HlslLibraryCompileOptions& options = {});
