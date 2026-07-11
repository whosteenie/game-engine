#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

class Camera;
class CascadedShadowMap;
class DirectionalShadowSettings;
class IBL;
class Mesh;
class SceneLighting;

class MeshShaderGBufferRenderer
{
public:
    MeshShaderGBufferRenderer();
    ~MeshShaderGBufferRenderer();

    MeshShaderGBufferRenderer(const MeshShaderGBufferRenderer&) = delete;
    MeshShaderGBufferRenderer& operator=(const MeshShaderGBufferRenderer&) = delete;

    bool IsSupported() const { return m_supported; }

    struct Batch
    {
        const Mesh* mesh = nullptr;
        std::uint32_t meshId = 0xFFFFFFFFu;
        std::vector<std::uint32_t> instanceIds;
    };

    struct SceneTables
    {
        std::uint64_t instanceTableGpuAddress = 0;
        std::uint64_t materialTableGpuAddress = 0;
    };

    // Per-light entry mirroring MeshLightData in mesh_lighting.hlsli. 16-byte-aligned so the C++
    // struct maps unambiguously onto the HLSL constant buffer (no scalar-array padding surprises).
    struct MeshLightData
    {
        glm::ivec4 typeAndFlags{0};
        glm::vec4 position{0.0f};
        glm::vec4 direction{0.0f};
        glm::vec4 color{0.0f};
        glm::vec4 params0{0.0f};
        glm::vec4 params1{0.0f};
    };

    // Mirror of MeshLightingConstants (cbuffer b1) in mesh_lighting.hlsli. Every member is a vec4 /
    // ivec4 / mat4 / array-of-those so the layout matches HLSL exactly.
    struct MeshLightingConstants
    {
        glm::vec4 viewPos{0.0f};
        glm::ivec4 lightMeta{0};
        MeshLightData lights[8];
        glm::mat4 lightSpaceMatrices[4]{glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
        glm::vec4 cascadeEndSplits{0.0f};
        glm::vec4 cascadeTexelWorldSizes{0.0f};
        glm::vec4 cascadeClipDepthMin{0.0f};
        glm::vec4 cascadeClipDepthMax{0.0f};
        glm::vec4 shadowParams0{0.0f};
        glm::vec4 shadowParams1{0.0f};
        glm::vec4 shadowParams2{0.0f};
        glm::vec4 shadowParams3{0.0f};
        glm::vec4 iblParams{0.0f};
        glm::vec4 irradianceSh[9];
    };

    // Per-frame lighting cbuffer + the shader-visible SRV heap indices for the shadow map array,
    // IBL prefilter cube, and BRDF LUT that the mesh-shader G-buffer PS samples. `valid` is false
    // when any required resource is unavailable (IBL not generated / no shadow depth) — the caller
    // must then fall back to the classic lit path rather than dispatch with unbound descriptors.
    struct MeshLightingBindings
    {
        MeshLightingConstants constants;
        std::uint32_t shadowSrvIndex = 0xFFFFFFFFu;
        std::uint32_t prefilterSrvIndex = 0xFFFFFFFFu;
        std::uint32_t brdfSrvIndex = 0xFFFFFFFFu;
        bool valid = false;
    };

    // Assemble the per-frame lighting bindings from the scene lighting state. Mirrors the constant
    // assembly in Material::Apply / SceneLighting::Apply / IBL::BindTextures for the classic path.
    MeshLightingBindings BuildLightingBindings(
        const Camera& camera,
        const SceneLighting& lighting,
        const CascadedShadowMap* shadowMap,
        const IBL& ibl,
        const DirectionalShadowSettings& shadowSettings) const;

    std::uint32_t DispatchMeshAssetBatch(
        const Batch& batch,
        const SceneTables& sceneTables,
        const MeshLightingBindings& lightingBindings,
        const Camera& camera,
        const glm::mat4& prevView,
        const glm::mat4& prevUnjitteredProjection,
        bool temporalHistoryValid) const;

private:
    void CreateRootSignature();
    void CreatePipelineState();

    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    bool m_supported = false;
};
