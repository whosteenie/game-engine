#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

class Mesh;

class MeshShaderShadowRenderer
{
public:
    MeshShaderShadowRenderer();
    ~MeshShaderShadowRenderer();

    MeshShaderShadowRenderer(const MeshShaderShadowRenderer&) = delete;
    MeshShaderShadowRenderer& operator=(const MeshShaderShadowRenderer&) = delete;

    bool IsSupported() const { return m_supported; }

    // One shared-mesh batch of shadow casters, pre-grouped by (meshId, doubleSided).
    struct Batch
    {
        const Mesh* mesh = nullptr;
        std::uint32_t meshId = 0xFFFFFFFFu;
        bool doubleSided = false;
        std::vector<std::uint32_t> instanceIds;
    };

    struct SceneTables
    {
        std::uint64_t instanceTableGpuAddress = 0;
    };

    // Cull (against the cascade frustum) + rasterize this batch's meshlets into the current cascade's
    // depth target. Returns total meshlet-instance work considered (for diagnostics).
    std::uint32_t DispatchMeshAssetBatch(
        const Batch& batch,
        const SceneTables& sceneTables,
        const glm::mat4& lightSpaceMatrix,
        const std::array<glm::vec4, 6>& cascadeFrustumPlanes,
        const glm::vec3& lightDirectionTowardSource,
        float casterDepthBias) const;

private:
    void CreateRootSignature();
    void CreatePipelineStates();

    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    void* m_pipelineStateDoubleSided = nullptr;
    bool m_supported = false;
};
