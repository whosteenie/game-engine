#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

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

    std::uint32_t DispatchMeshAssetBatch(
        const Batch& batch,
        const SceneTables& sceneTables,
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
