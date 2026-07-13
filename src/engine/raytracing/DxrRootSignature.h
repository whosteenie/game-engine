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

// Phase D4 reflections (see devdoc/dxr/reflections.md). Layout mirrors the cbuffer in
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
    std::uint32_t aoRayCount = 4; // reflected-hit AO ray count (0 = off), tunable
    float irradianceSh9[9][4] = {}; // L2 SH diffuse irradiance (9 x float4)
    // Surfaces rougher than this skip the scattered reflection trace and write the prefiltered-env
    // fallback (see g_RoughnessCutoff in reflections.hlsl). GI is unaffected (ignores this field).
    float roughnessCutoff = 0.6f;
    float sunAngularTanRadius = 0.0f;
    float giStrength = 1.0f;
    std::uint32_t hasGiTrace = 0;
    // Path-tracer-only (P4): unjittered matrices for primary-hit motion vectors (DLSS-RR).
    float paddingUnjitteredViewProj[4] = {};
    float unjitteredViewProj[16] = {};
    float prevViewProj[16] = {};
    // F2 path-tracer-only: emissive NEE light list (t15). Zero for non-PT dispatches.
    std::uint32_t emissiveLightCount = 0;
    float emissiveLightPickWeightSum = 0.0f;
    float paddingPtEmissiveTransport = 0.0f;
    float ptDebugIsolateMode = 0.0f; // path-tracer-only radiance term isolation (see RenderDebug.h)
    // PT-A transmission virtual motion: previous unjittered frustum + camera for dual-frame refract MV.
    float prevInvViewProj[16] = {};
    float prevCameraPos[3] = {};
    // Path-tracer-only: 1 when any dielectric can refract NEE shadows (else opaque any-hit).
    float sceneHasTransmission = 0.0f;
    // F2 path-tracer-only: environment importance sampling (t16 CDF + t17 HDR equirect).
    std::uint32_t envLightImportanceCount = 0;
    std::uint32_t envIsCdfWidth = 0;
    float envDirectLightingLuminanceClamp = 0.0f;
    std::uint32_t envIsCdfHeight = 0;
    // ReSTIR DI initial sampling (roadmap P2): per-category candidate count. 0 = off (plain NEE).
    // 1 = one emissive + one env candidate — byte-exact parity with DI off (A/B validation anchor).
    // N>1 = RIS over N candidates with one shadow ray each (lower variance, same expected value).
    float restirDiCandidateCount = 0.0f;
    float restirGiInitialEnabled = 0.0f;
    float _restirDiPad1 = 0.0f;
    float _restirDiPad2 = 0.0f;
};

// Phase D8 shadows (see devdoc/dxr/shadows.md). Layout mirrors the cbuffer in
// assets/shaders/dxr/shadows.hlsl exactly (16-byte HLSL packing: matrices first, then
// float3+float pairs, then scalars in multiples of 4).
struct RestirTemporalConstants
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t historyValid = 0;
    std::uint32_t frameIndex = 0;
    float invViewProj[16] = {};
    float cameraPos[3] = {};
    float maxTraceDistance = 100.0f;
    float prevCameraPos[3] = {};
    float spatialFilterStrength = 0.2f;
    std::uint32_t shadeOutput = 1; // rewrite g_Output = direct + Y·W (0 keeps isolate AOVs)
    std::uint32_t spatialSampleCount = 5;
    float spatialRadius = 20.0f;
    std::uint32_t spatialIteration = 0;
    std::uint32_t emissiveLightCount = 0;
    float emissiveLightPickWeightSum = 0.0f;
    std::uint32_t envImportanceCount = 0;
    std::uint32_t envCdfWidth = 0;
    std::uint32_t envCdfHeight = 0;
    float environmentIntensity = 1.0f;
    float envDirectLuminanceClamp = 0.0f;
    float analyticSunActive = 0.0f;
    float sunDirection[3] = {0.0f, 1.0f, 0.0f};
    float sunAngularTanRadius = 0.0f;
    std::uint32_t debugMode = 0;
    std::uint32_t enableDiTemporal = 0;
    std::uint32_t enableGiTemporal = 0;
    std::uint32_t _padDebug = 0;
};
static_assert(sizeof(RestirTemporalConstants) == 192, "ReSTIR DI temporal cbuffer layout mismatch");

struct ShadowDispatchConstants
{
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t gbufferWidth = 0;
    std::uint32_t gbufferHeight = 0;
    float invViewProj[16] = {};
    float worldToView[16] = {};
    float cameraPos[3] = {};
    float sunAngularTanRadius = 0.0f;
    float sunDirection[3] = {0.0f, 1.0f, 0.0f};
    float maxTraceDistance = 100.0f;
    std::uint32_t frameIndex = 0;
    std::uint32_t _pad0 = 0;
    std::uint32_t _pad1 = 0;
    std::uint32_t _pad2 = 0;
};

void SerializeSmokeGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeSmokeLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePrimaryDebugLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeReflectionGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializePathTracerGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeShadowGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
ID3D12RootSignature* CreateSmokeGlobalRootSignature();
ID3D12RootSignature* CreateSmokeLocalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugGlobalRootSignature();
ID3D12RootSignature* CreatePrimaryDebugLocalRootSignature();
ID3D12RootSignature* CreateReflectionGlobalRootSignature();
ID3D12RootSignature* CreateReflectionLocalRootSignature();
ID3D12RootSignature* CreatePathTracerGlobalRootSignature();
ID3D12RootSignature* CreateShadowGlobalRootSignature();
ID3D12RootSignature* CreateShadowLocalRootSignature();
void SerializeRestirGlobalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
void SerializeRestirLocalRootSignature(Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
ID3D12RootSignature* CreateRestirGlobalRootSignature();
ID3D12RootSignature* CreateRestirLocalRootSignature();
void ReleaseSmokeGlobalRootSignature();
void ReleaseSmokeLocalRootSignature();
void ReleasePrimaryDebugGlobalRootSignature();
void ReleasePrimaryDebugLocalRootSignature();
void ReleaseReflectionGlobalRootSignature();
void ReleaseReflectionLocalRootSignature();
void ReleasePathTracerGlobalRootSignature();
void ReleaseShadowGlobalRootSignature();
void ReleaseShadowLocalRootSignature();
void ReleaseRestirGlobalRootSignature();
void ReleaseRestirLocalRootSignature();

} // namespace DxrRootSignature
