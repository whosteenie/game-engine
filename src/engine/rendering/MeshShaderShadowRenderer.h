#pragma once

#include <glm/glm.hpp>

#include <cstdint>

class Mesh;

class MeshShaderShadowRenderer
{
public:
    MeshShaderShadowRenderer();
    ~MeshShaderShadowRenderer();

    MeshShaderShadowRenderer(const MeshShaderShadowRenderer&) = delete;
    MeshShaderShadowRenderer& operator=(const MeshShaderShadowRenderer&) = delete;

    bool IsSupported() const { return m_supported; }

    std::uint32_t DispatchMesh(
        const Mesh& mesh,
        const glm::mat4& model,
        const glm::mat4& lightSpaceMatrix,
        const glm::vec3& lightDirectionTowardSource,
        float casterDepthBias,
        bool doubleSided) const;

private:
    void CreateRootSignature();
    void CreatePipelineStates();

    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    void* m_pipelineStateDoubleSided = nullptr;
    bool m_supported = false;
};
