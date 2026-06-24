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

HlslCompileResult CompileHlsl(
    const std::string& source,
    const std::string& sourcePath,
    const char* entry,
    const char* targetProfile);
