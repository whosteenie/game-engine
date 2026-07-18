#pragma once

#include "engine/rhi/d3d12/HlslCompiler.h"

#include <memory>
#include <string>
#include <vector>

struct DxrCompiledLibrary
{
    Microsoft::WRL::ComPtr<IDxcBlob> containerBytecode;
    Microsoft::WRL::ComPtr<IDxcBlob> dxilBytecode;
    bool extractedFromDxbcContainer = false;
    std::string sourcePath;
};

struct DxrShaderLibraryCompileOptions
{
    std::string targetProfile = "lib_6_3";
    std::vector<std::string> featureDefines;
    bool diagnosticPermutation = false;
    // Explicit feature states participating in the library cache key.
    bool serPermutation = false;
    bool inlineVisibilityPermutation = false;
};

class DxrShaderCache
{
public:
    // Path tracer diagnostics are a separate compile-time permutation. Other libraries use normal.
    static std::shared_ptr<DxrCompiledLibrary> Load(const char* libraryPath, bool diagnosticPermutation = false);
    static std::shared_ptr<DxrCompiledLibrary> Load(
        const char* libraryPath,
        const DxrShaderLibraryCompileOptions& options);
    static DxrShaderLibraryCompileOptions MakeActiveDeviceCompileOptions(bool diagnosticPermutation = false);
    static void Clear();

private:
    DxrShaderCache() = default;

    static std::string ReadShaderSource(const char* libraryPath);
};
