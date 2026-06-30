#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

struct ID3D12RootSignature;

namespace DxrRootSignature
{
struct DispatchConstants
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t _padding0 = 0;
    std::uint32_t _padding1 = 0;
    float clearColor[4] = {1.0f, 0.0f, 1.0f, 1.0f};
};

struct PrimaryDispatchConstants
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t _padding0 = 0;
    std::uint32_t _padding1 = 0;
    float invViewProj[16] = {};
    float viewProj[16] = {};
    float cameraPos[3] = {};
    float _padding2 = 0.0f;
    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
    float maxTraceDistance = 100.0f;
    float _padding3 = 0.0f;
};

void SerializeSmokeGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeSmokeLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
ID3D12RootSignature* CreateSmokeGlobalRootSignature();
ID3D12RootSignature* CreateSmokeLocalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugGlobalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugLocalRootSignature();
void ReleaseSmokeGlobalRootSignature();
void ReleaseSmokeLocalRootSignature();
void ReleasePrimaryDebugGlobalRootSignature();
void ReleasePrimaryDebugLocalRootSignature();

} // namespace DxrRootSignature
