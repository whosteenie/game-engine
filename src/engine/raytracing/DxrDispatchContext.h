#pragma once

#include "engine/raytracing/DxrRootSignature.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;
struct ID3D12StateObject;
struct ID3D12RootSignature;

namespace D3D12MA
{
class Allocation;
}

class DxrPipeline;
class ShaderBindingTable;

class DxrDispatchContext
{
public:
    DxrDispatchContext() = default;
    ~DxrDispatchContext();

    DxrDispatchContext(const DxrDispatchContext&) = delete;
    DxrDispatchContext& operator=(const DxrDispatchContext&) = delete;

    bool EnsureOutput(int width, int height, std::string& outError);
    void Release();

    bool DispatchSmoke(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        int width,
        int height,
        const DxrRootSignature::DispatchConstants& constants,
        std::string& outError);

    bool DispatchPrimaryDebug(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        std::uintptr_t depthSrvCpuHandle,
        std::uint32_t geometryLookupSrvIndex,
        std::uint32_t sceneVertexFloatsSrvIndex,
        std::uint32_t sceneIndicesSrvIndex,
        int width,
        int height,
        const DxrRootSignature::PrimaryDispatchConstants& constants,
        std::string& outError);

    // Phase D4 reflections (devdoc/dxr/reflections.md). SRV CPU handles must live in the
    // shader-visible SRV heap (same contract as depthSrvCpuHandle above).
    struct ReflectionDispatchInputs
    {
        ID3D12Resource* tlasResource = nullptr;
        std::uint64_t tlasGpuVirtualAddress = 0;
        std::uintptr_t depthSrvCpuHandle = 0;
        std::uintptr_t normalSrvCpuHandle = 0;
        std::uintptr_t material0SrvCpuHandle = 0;
        std::uint32_t geometryLookupSrvIndex = UINT32_MAX;
        std::uint32_t sceneVertexFloatsSrvIndex = UINT32_MAX;
        std::uint32_t sceneIndicesSrvIndex = UINT32_MAX;
        std::uint32_t materialSrvIndex = UINT32_MAX; // per-object material table (t12)
        std::uintptr_t directSrvCpuHandle = 0;
        std::uintptr_t sunShadowSrvCpuHandle = 0;
        std::uintptr_t indirectSrvCpuHandle = 0;
        std::uintptr_t prefilterSrvCpuHandle = 0;
        std::uintptr_t velocitySrvCpuHandle = 0; // RT4, NRD motion guide source
        std::uintptr_t giDenoisedSrvCpuHandle = 0; // RELAX_DIFFUSE for screen-space hit lookup (t13)
        // P4b path tracer only (t14): per-instance previous object-to-world rows for object
        // motion vectors. UINT32_MAX = unavailable (shader falls back to camera-only reprojection).
        std::uint32_t prevInstanceTransformsSrvIndex = UINT32_MAX;
        // F2 path tracer only (t15): emissive instance list for NEE. UINT32_MAX = unavailable.
        std::uint32_t emissiveLightsSrvIndex = UINT32_MAX;
    };

    // Phase D5 (devdoc/dxr/nrd-integration.md): everything the NRD backend needs to denoise
    // this frame's reflection trace. States are D3D12_RESOURCE_STATES of each resource as
    // left by DispatchReflections; the denoiser transitions internally and reports back.
    struct ReflectionNrdResources
    {
        ID3D12Resource* radianceHitDist = nullptr;  // IN_SPEC_RADIANCE_HITDIST (RGBA16F)
        ID3D12Resource* viewZ = nullptr;            // IN_VIEWZ (R32F)
        ID3D12Resource* normalRoughness = nullptr;  // IN_NORMAL_ROUGHNESS (RGBA8)
        ID3D12Resource* motion = nullptr;           // IN_MV (RG16F)
        ID3D12Resource* denoisedOutput = nullptr;   // OUT_SPEC_RADIANCE_HITDIST (RGBA16F)
        std::uint32_t* radianceState = nullptr;
        std::uint32_t* viewZState = nullptr;
        std::uint32_t* normalRoughnessState = nullptr;
        std::uint32_t* motionState = nullptr;
        std::uint32_t* denoisedState = nullptr;
        int textureWidth = 0;
        int textureHeight = 0;
        int dispatchWidth = 0;
        int dispatchHeight = 0;
    };

    // Valid after a successful DispatchReflections this frame.
    ReflectionNrdResources GetReflectionNrdResources();
    std::uintptr_t GetReflectionDenoisedSrvCpuHandle() const { return m_reflectionDenoisedSrvCpuHandle; }

    bool DispatchReflections(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        const ReflectionDispatchInputs& inputs,
        int width,
        int height,
        const DxrRootSignature::ReflectionDispatchConstants& constants,
        std::string& outError);

    // Phase P1 path tracer (devdoc/dxr/path-tracing.md). Reuses the reflection global root
    // signature bindings but writes HDR radiance to the primary-debug output textures (u0/u1).
    bool DispatchPathTracer(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        const ReflectionDispatchInputs& inputs,
        int width,
        int height,
        const DxrRootSignature::ReflectionDispatchConstants& constants,
        std::string& outError);

    // Phase D8 shadows (devdoc/dxr/shadows.md). SRV CPU handles must live in the shader-visible
    // SRV heap (same contract as the reflection inputs).
    struct ShadowDispatchInputs
    {
        ID3D12Resource* tlasResource = nullptr;
        std::uint64_t tlasGpuVirtualAddress = 0;
        std::uintptr_t depthSrvCpuHandle = 0;
        std::uintptr_t normalSrvCpuHandle = 0;    // RT2 shading normal
        std::uintptr_t material0SrvCpuHandle = 0; // RT5 roughness in .a (normal-roughness guide)
        std::uintptr_t velocitySrvCpuHandle = 0;  // RT4, NRD motion guide source
    };

    // Everything the NRD SIGMA_SHADOW backend needs to denoise this frame's penumbra buffer.
    struct ShadowNrdResources
    {
        ID3D12Resource* penumbra = nullptr;        // IN_PENUMBRA (R16F)
        ID3D12Resource* viewZ = nullptr;           // IN_VIEWZ (R32F)
        ID3D12Resource* normalRoughness = nullptr; // IN_NORMAL_ROUGHNESS (RGBA16_UNORM)
        ID3D12Resource* motion = nullptr;          // IN_MV (RG16F)
        ID3D12Resource* denoisedOutput = nullptr;  // OUT_SHADOW_TRANSLUCENCY (R16F, also history)
        std::uint32_t* penumbraState = nullptr;
        std::uint32_t* viewZState = nullptr;
        std::uint32_t* normalRoughnessState = nullptr;
        std::uint32_t* motionState = nullptr;
        std::uint32_t* denoisedState = nullptr;
        int textureWidth = 0;
        int textureHeight = 0;
        int dispatchWidth = 0;
        int dispatchHeight = 0;
    };

    // Valid after a successful DispatchShadows this frame.
    ShadowNrdResources GetShadowNrdResources();
    std::uintptr_t GetShadowPenumbraSrvCpuHandle() const { return m_shadowPenumbraSrvCpuHandle; }
    std::uintptr_t GetShadowDenoisedSrvCpuHandle() const { return m_shadowDenoisedSrvCpuHandle; }
    int GetShadowOutputWidth() const { return m_shadowOutputWidth; }
    int GetShadowOutputHeight() const { return m_shadowOutputHeight; }
    int GetShadowDispatchWidth() const { return m_shadowDispatchWidth; }
    int GetShadowDispatchHeight() const { return m_shadowDispatchHeight; }

    bool DispatchShadows(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        const ShadowDispatchInputs& inputs,
        int width,
        int height,
        const DxrRootSignature::ShadowDispatchConstants& constants,
        std::string& outError);

    // Phase D9 diffuse GI (devdoc/dxr/diffuse-gi.md). Reuses the reflection inputs/root signature
    // and the ReflectionNrdResources layout (RELAX_DIFFUSE denoiser, separate instance).
    ReflectionNrdResources GetGiNrdResources();
    std::uintptr_t GetGiOutputSrvCpuHandle() const { return m_giOutputSrvCpuHandle; }
    std::uintptr_t GetGiDenoisedSrvCpuHandle() const { return m_giDenoisedSrvCpuHandle; }
    int GetGiOutputWidth() const { return m_giOutputWidth; }
    int GetGiOutputHeight() const { return m_giOutputHeight; }
    int GetGiDispatchWidth() const { return m_giDispatchWidth; }
    int GetGiDispatchHeight() const { return m_giDispatchHeight; }

    bool DispatchGi(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const ShaderBindingTable& shaderBindingTable,
        const ReflectionDispatchInputs& inputs,
        int width,
        int height,
        const DxrRootSignature::ReflectionDispatchConstants& constants,
        std::string& outError);

    std::uintptr_t GetOutputSrvCpuHandle() const { return m_outputSrvCpuHandle; }
    std::uintptr_t GetPrimaryOutputSrvCpuHandle() const { return m_primaryOutputSrvCpuHandle; }
    std::uintptr_t GetPrimaryMetadataSrvCpuHandle() const { return m_primaryMetadataSrvCpuHandle; }
    ID3D12Resource* GetPrimaryOutputResource() const { return m_primaryOutputResource; }
    std::uint32_t GetPrimaryOutputResourceState() const { return m_primaryOutputResourceState; }
    ID3D12Resource* GetPathTracerDepthResource() const { return m_ptDepthTexture.resource; }
    std::uint32_t GetPathTracerDepthResourceState() const { return m_ptDepthTexture.state; }
    // R32 primary-depth SRV — P4 resolves this into a D24 target for the DLSS depth input.
    std::uintptr_t GetPathTracerDepthSrvCpuHandle() const { return m_ptDepthTexture.srvCpuHandle; }
    std::uintptr_t GetPathTracerMotionSrvCpuHandle() const { return m_ptMotionTexture.srvCpuHandle; }
    ID3D12Resource* GetPathTracerMotionResource() const { return m_ptMotionTexture.resource; }
    std::uint32_t GetPathTracerMotionResourceState() const { return m_ptMotionTexture.state; }
    // P4b bounce-0 RR material guides (u4-u6), left in pixel/non-pixel shader read after dispatch.
    std::uintptr_t GetPathTracerDiffuseAlbedoSrvCpuHandle() const { return m_ptDiffuseAlbedoTexture.srvCpuHandle; }
    std::uintptr_t GetPathTracerSpecularAlbedoSrvCpuHandle() const { return m_ptSpecularAlbedoTexture.srvCpuHandle; }
    std::uintptr_t GetPathTracerNormalRoughnessSrvCpuHandle() const { return m_ptNormalRoughnessTexture.srvCpuHandle; }
    std::uintptr_t GetReflectionOutputSrvCpuHandle() const { return m_reflectionOutputSrvCpuHandle; }
    int GetReflectionOutputWidth() const { return m_reflectionOutputWidth; }
    int GetReflectionOutputHeight() const { return m_reflectionOutputHeight; }
    // Last DispatchRays grid size — may be smaller than the texture when a larger output is
    // kept alive (quality shrink / mixed viewport sizes). Consumers must scale UVs by
    // dispatch/texture size or they read stale texels (picture-in-picture artifact).
    int GetReflectionDispatchWidth() const { return m_reflectionDispatchWidth; }
    int GetReflectionDispatchHeight() const { return m_reflectionDispatchHeight; }
    int GetOutputWidth() const { return m_outputWidth; }
    int GetOutputHeight() const { return m_outputHeight; }
    ID3D12Resource* GetOutputResource() const { return m_outputResource; }

private:
    bool CreateTlasSrv(
        ID3D12Resource* tlasResource,
        std::uint64_t tlasGpuVirtualAddress,
        std::string& outError);
    void CreateOutputDescriptors();
    bool EnsurePrimaryOutput(int width, int height, std::string& outError);
    bool EnsurePathTracerGuides(int width, int height, std::string& outError);
    void CreatePrimaryOutputDescriptors();
    bool EnsureReflectionOutput(int width, int height, std::string& outError);
    bool EnsureShadowOutput(int width, int height, std::string& outError);
    bool EnsureGiOutput(int width, int height, std::string& outError);
    void ReleaseRetiredOutputs();
    void ReleaseRetiredPrimaryOutputs();
    void ReleaseRetiredReflectionOutputs();
    void ReleaseRetiredShadowOutputs();
    void ReleaseRetiredGiOutputs();
    std::uint32_t DepthSrvIndexFromCpuHandle(std::uintptr_t depthSrvCpuHandle) const;

    struct RetiredOutput
    {
        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uint32_t uavIndex = UINT32_MAX;
    };

    ID3D12Resource* m_outputResource = nullptr;
    D3D12MA::Allocation* m_outputAllocation = nullptr;

    std::uint32_t m_tlasSrvIndex = UINT32_MAX;
    std::uint32_t m_outputUavIndex = UINT32_MAX;
    std::uint32_t m_outputSrvIndex = UINT32_MAX;
    std::uintptr_t m_outputSrvCpuHandle = 0;
    std::uint32_t m_outputResourceState = 0;
    int m_outputWidth = 0;
    int m_outputHeight = 0;

    std::vector<RetiredOutput> m_retiredOutputs;

    ID3D12Resource* m_primaryOutputResource = nullptr;
    D3D12MA::Allocation* m_primaryOutputAllocation = nullptr;
    ID3D12Resource* m_primaryMetadataResource = nullptr;
    D3D12MA::Allocation* m_primaryMetadataAllocation = nullptr;

    std::uint32_t m_primaryOutputUavIndex = UINT32_MAX;
    std::uint32_t m_primaryOutputSrvIndex = UINT32_MAX;
    std::uint32_t m_primaryMetadataUavIndex = UINT32_MAX;
    std::uint32_t m_primaryMetadataSrvIndex = UINT32_MAX;
    std::uintptr_t m_primaryOutputSrvCpuHandle = 0;
    std::uintptr_t m_primaryMetadataSrvCpuHandle = 0;
    std::uint32_t m_primaryOutputResourceState = 0;
    std::uint32_t m_primaryMetadataResourceState = 0;
    int m_primaryOutputWidth = 0;
    int m_primaryOutputHeight = 0;

    std::vector<RetiredOutput> m_retiredPrimaryOutputs;
    std::vector<RetiredOutput> m_retiredPrimaryMetadata;

    // One quality-scaled texture in the reflection set (trace output, NRD guides, denoised).
    struct ReflectionTexture
    {
        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uint32_t uavIndex = UINT32_MAX;
        std::uintptr_t srvCpuHandle = 0;
        std::uint32_t state = 0;
    };

    bool CreateReflectionTexture(
        int width,
        int height,
        std::uint32_t dxgiFormat,
        ReflectionTexture& outTexture,
        std::string& outError);
    void RetireOrDestroyReflectionTexture(ReflectionTexture& texture);

    // P4 path-tracer DLSS guides (primary-hit depth + motion).
    ReflectionTexture m_ptDepthTexture{};
    ReflectionTexture m_ptMotionTexture{};
    // P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md).
    ReflectionTexture m_ptDiffuseAlbedoTexture{};   // RGBA8: albedo·(1−metallic)
    ReflectionTexture m_ptSpecularAlbedoTexture{};  // RGBA8: EnvBRDFApprox2(F0, roughness², NoV)
    ReflectionTexture m_ptNormalRoughnessTexture{}; // RGBA16F: world normal xyz + roughness w

    static constexpr int kReflectionTextureCount = 5;
    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised
    ReflectionTexture m_reflectionTextures[kReflectionTextureCount]{};
    std::uintptr_t m_reflectionOutputSrvCpuHandle = 0;  // alias of [0]
    std::uintptr_t m_reflectionDenoisedSrvCpuHandle = 0; // alias of [4]
    int m_reflectionOutputWidth = 0;
    int m_reflectionOutputHeight = 0;
    int m_reflectionDispatchWidth = 0;
    int m_reflectionDispatchHeight = 0;
    std::vector<RetiredOutput> m_retiredReflectionOutputs;

    // Phase D8 shadow texture set (reuses the ReflectionTexture struct + create/retire helpers).
    // [0] penumbra, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised (OUT_SHADOW_TRANSLUCENCY).
    static constexpr int kShadowTextureCount = 5;
    ReflectionTexture m_shadowTextures[kShadowTextureCount]{};
    std::uintptr_t m_shadowPenumbraSrvCpuHandle = 0;  // alias of [0]
    std::uintptr_t m_shadowDenoisedSrvCpuHandle = 0;  // alias of [4]
    int m_shadowOutputWidth = 0;
    int m_shadowOutputHeight = 0;
    int m_shadowDispatchWidth = 0;
    int m_shadowDispatchHeight = 0;
    std::vector<RetiredOutput> m_retiredShadowOutputs;

    // Phase D9 diffuse-GI texture set (reuses ReflectionTexture + create/retire helpers).
    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    static constexpr int kGiTextureCount = 5;
    ReflectionTexture m_giTextures[kGiTextureCount]{};
    std::uintptr_t m_giOutputSrvCpuHandle = 0;   // alias of [0]
    std::uintptr_t m_giDenoisedSrvCpuHandle = 0; // alias of [4]
    int m_giOutputWidth = 0;
    int m_giOutputHeight = 0;
    int m_giDispatchWidth = 0;
    int m_giDispatchHeight = 0;
    std::vector<RetiredOutput> m_retiredGiOutputs;
};
