#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

class Camera;
class Mesh;

class MeshShaderGBufferRenderer
{
public:
    MeshShaderGBufferRenderer();
    ~MeshShaderGBufferRenderer();

    MeshShaderGBufferRenderer(const MeshShaderGBufferRenderer&) = delete;
    MeshShaderGBufferRenderer& operator=(const MeshShaderGBufferRenderer&) = delete;

    bool IsSupported() const { return m_supported; }

    std::uint32_t DispatchMesh(
        const Mesh& mesh,
        const Camera& camera,
        const glm::mat4& model,
        const glm::mat4& prevModel,
        const glm::mat4& prevView,
        const glm::mat4& prevUnjitteredProjection,
        bool temporalHistoryValid,
        const glm::vec3& albedo,
        float roughness,
        float metallic) const;

private:
    void CreateRootSignature();
    void CreatePipelineState();

    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    bool m_supported = false;
};
