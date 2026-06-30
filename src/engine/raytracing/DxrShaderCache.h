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
    static std::shared_ptr<DxrCompiledLibrary> Load(const char* libraryPath);
    static void Clear();

private:
    DxrShaderCache() = default;

    static std::string ReadShaderSource(const char* libraryPath);
};
