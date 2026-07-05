#pragma once

#include "engine/raytracing/DxrDispatchContext.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D12RootSignature;

namespace D3D12MA
{
class Allocation;
}

namespace nrd
{
struct Instance;
}

// Phase D5 — NVIDIA NRD (RELAX_SPECULAR) manual D3D12 backend, no NRI
// (devdoc/dxr-nrd-integration.md). NRD is API-agnostic: it hands us compute-shader DXIL,
// resource pool descriptions, and per-frame dispatch lists; we own PSOs, pool textures,
// barriers, and descriptor binding. Written against NRD v4.17 (pinned in CMakeLists.txt).
class NrdDenoiser
{
public:
    struct FrameParameters
    {
        glm::mat4 viewToClip{1.0f};       // non-jittered projection
        glm::mat4 viewToClipPrev{1.0f};
        glm::mat4 worldToView{1.0f};
        glm::mat4 worldToViewPrev{1.0f};
        glm::vec2 cameraJitterUv{0.0f};   // [-0.5; 0.5] pixel-UV jitter
        glm::vec2 cameraJitterUvPrev{0.0f};
        std::uint32_t frameIndex = 0;
        float denoisingRange = 500.0f;    // world units; sky viewZ must exceed this
        float temporalBlend = 0.95f;      // engine setting, mapped to accumulated frames
        int atrousIterations = 5;         // RELAX spatial A-trous passes [2; 8]
        bool antiFirefly = true;          // RELAX anti-firefly spatial suppression
        bool resetHistory = false;
    };

    NrdDenoiser() = default;
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&) = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    // Records the full RELAX_SPECULAR dispatch chain onto the command list.
    // Resource states in "resources" are transitioned as needed and written back.
    bool Denoise(
        ID3D12GraphicsCommandList* commandList,
        const DxrDispatchContext::ReflectionNrdResources& resources,
        const FrameParameters& frame,
        std::string& outError);

    void Release();
    bool IsReady() const { return m_instance != nullptr; }
    const std::string& GetLastError() const { return m_lastError; }

private:
    struct PoolTexture
    {
        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        std::uint32_t dxgiFormat = 0;
        std::uint32_t state = 0;
        int width = 0;
        int height = 0;
    };

    bool EnsureInstance(int resourceWidth, int resourceHeight, std::string& outError);
    bool CreatePipelines(std::string& outError);
    bool CreatePoolTextures(int resourceWidth, int resourceHeight, std::string& outError);
    void ReleasePoolTextures();

    // Resolves an nrd::ResourceDesc to the backing engine/pool resource + tracked state.
    bool ResolveResource(
        const void* nrdResourceDesc, // const nrd::ResourceDesc*
        const DxrDispatchContext::ReflectionNrdResources& resources,
        ID3D12Resource*& outResource,
        std::uint32_t*& outState,
        std::uint32_t& outDxgiFormat) const;

    nrd::Instance* m_instance = nullptr;
    ID3D12RootSignature* m_rootSignature = nullptr;
    std::vector<ID3D12PipelineState*> m_pipelines;
    std::vector<PoolTexture> m_permanentPool;
    std::vector<PoolTexture> m_transientPool;
    // Engine resource states are owned by the caller; pool states live in the pools above.
    // Format lookups for creating per-dispatch descriptors of engine resources.
    mutable std::unordered_map<ID3D12Resource*, std::uint32_t> m_engineResourceFormats;

    int m_resourceWidth = 0;
    int m_resourceHeight = 0;
    std::uint32_t m_srvTableSize = 0; // per-set maximums from InstanceDesc
    std::uint32_t m_uavTableSize = 0;
    std::uint32_t m_constantBufferMaxDataSize = 0;
    std::uint32_t m_cbRegister = 0;
    std::uint32_t m_cbSpace = 0;
    std::uint32_t m_samplersBaseRegister = 0;
    std::uint32_t m_resourcesBaseRegister = 0;
    std::uint32_t m_resourcesSpace = 0;
    std::string m_lastError;
    bool m_creationFailed = false; // don't retry every frame after a hard failure
};
