#pragma once

#include "engine/rhi/d3d12/HlslCompiler.h"

#include <memory>
#include <string>

struct DxrCompiledLibrary
{
    Microsoft::WRL::ComPtr<IDxcBlob> containerBytecode;
    Microsoft::WRL::ComPtr<IDxcBlob> dxilBytecode;
    bool extractedFromDxbcContainer = false;
    std::string sourcePath;
};

class DxrShaderCache
{
public:
    // Path tracer diagnostics are a separate compile-time permutation. Other libraries use normal.
    static std::shared_ptr<DxrCompiledLibrary> Load(const char* libraryPath, bool diagnosticPermutation = false);
    static void Clear();

private:
    DxrShaderCache() = default;

    static std::string ReadShaderSource(const char* libraryPath);
};
