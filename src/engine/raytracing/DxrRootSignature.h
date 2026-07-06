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

// Phase D4 reflections (see devdoc/dxr-reflections.md). Layout mirrors the cbuffer in
// assets/shaders/dxr/reflections.hlsl exactly.
struct ReflectionDispatchConstants
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t gbufferWidth = 0;
    std::uint32_t gbufferHeight = 0;
    float invViewProj[16] = {};
    float viewProj[16] = {};
    float worldToView[16] = {};
    float cameraPos[3] = {};
    float maxTraceDistance = 100.0f;
    float environmentIntensity = 1.0f;
    float maxReflectionLod = 4.0f;
    std::uint32_t frameIndex = 0;
    std::uint32_t samplesPerPixel = 1;
    // In-hit analytic shading inputs (see assets/shaders/dxr/reflections.hlsl).
    float sunDirection[3] = {0.0f, -1.0f, 0.0f};
    float sunIntensity = 0.0f;
    float sunColor[3] = {1.0f, 1.0f, 1.0f};
    float _padSun = 0.0f;
    float irradianceSh9[9][4] = {}; // L2 SH diffuse irradiance (9 x float4)
};

void SerializeSmokeGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeSmokeLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeReflectionGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
ID3D12RootSignature* CreateSmokeGlobalRootSignature();
ID3D12RootSignature* CreateSmokeLocalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugGlobalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugLocalRootSignature();
ID3D12RootSignature* CreateReflectionGlobalRootSignature();
ID3D12RootSignature* CreateReflectionLocalRootSignature();
void ReleaseSmokeGlobalRootSignature();
void ReleaseSmokeLocalRootSignature();
void ReleasePrimaryDebugGlobalRootSignature();
void ReleasePrimaryDebugLocalRootSignature();
void ReleaseReflectionGlobalRootSignature();
void ReleaseReflectionLocalRootSignature();

} // namespace DxrRootSignature
