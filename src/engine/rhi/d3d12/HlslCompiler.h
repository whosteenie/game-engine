#pragma once

#include <d3d12shader.h>
#include <dxcapi.h>

#include <string>

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

DxilLibraryBytecode PrepareDxilLibraryBytecode(Microsoft::WRL::ComPtr<IDxcBlob> dxcOutput);

HlslCompileResult CompileHlsl(
    const std::string& source,
    const std::string& sourcePath,
    const char* entry,
    const char* targetProfile);

HlslCompileResult CompileHlslLibrary(
    const std::string& source,
    const std::string& sourcePath,
    const char* define = nullptr);
