#include "engine/rendering/GridRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

#include <cmath>
#include <glm/glm.hpp>
#include <vector>

GridRenderer::GridRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::GridFragmentShader))
{
    BuildGridGeometry(m_halfExtent);
}

GridRenderer::~GridRenderer()
{
    ReleaseGpuResources();
}

GridRenderer::GridRenderer(GridRenderer&& other) noexcept
    : m_shader(std::move(other.m_shader)),
      m_vertexBuffer(std::move(other.m_vertexBuffer)),
      m_indexBuffer(std::move(other.m_indexBuffer)),
      m_indexCount(other.m_indexCount),
      m_halfExtent(other.m_halfExtent),
      m_cellSize(other.m_cellSize),
      m_majorInterval(other.m_majorInterval),
      m_gridHeight(other.m_gridHeight),
      m_fadeStartFraction(other.m_fadeStartFraction),
      m_fadeEndFraction(other.m_fadeEndFraction)
{
    other.m_indexCount = 0;
}

GridRenderer& GridRenderer::operator=(GridRenderer&& other) noexcept
{
    if (this != &other)
    {
        ReleaseGpuResources();
        m_shader = std::move(other.m_shader);
        m_vertexBuffer = std::move(other.m_vertexBuffer);
        m_indexBuffer = std::move(other.m_indexBuffer);
        m_indexCount = other.m_indexCount;
        m_halfExtent = other.m_halfExtent;
        m_cellSize = other.m_cellSize;
        m_majorInterval = other.m_majorInterval;
        m_gridHeight = other.m_gridHeight;
        m_fadeStartFraction = other.m_fadeStartFraction;
        m_fadeEndFraction = other.m_fadeEndFraction;
        other.m_indexCount = 0;
    }

    return *this;
}

void GridRenderer::ReleaseGpuResources()
{
    m_vertexBuffer.Destroy();
    m_indexBuffer.Destroy();
    m_indexCount = 0;
}

void GridRenderer::BuildGridGeometry(float halfExtent)
{
    const std::vector<float> vertices = {
        -halfExtent, 0.0f, -halfExtent,
         halfExtent, 0.0f, -halfExtent,
         halfExtent, 0.0f,  halfExtent,
        -halfExtent, 0.0f,  halfExtent,
    };

    const std::vector<unsigned int> indices = {
        0, 1, 2,
        0, 2, 3,
    };

    m_indexCount = static_cast<unsigned int>(indices.size());

    m_vertexBuffer.Create(
        GpuBuffer::Type::Vertex,
        vertices.data(),
        static_cast<std::uint32_t>(vertices.size() * sizeof(float)));
    m_indexBuffer.Create(
        GpuBuffer::Type::Index,
        indices.data(),
        static_cast<std::uint32_t>(indices.size() * sizeof(unsigned int)));
}

void GridRenderer::Draw(const Camera& camera, const bool outputLinear) const
{
    if (!m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid() || m_indexCount == 0)
    {
        return;
    }

    const glm::vec3 cameraPosition = camera.GetPosition();
    const float patchSize = m_halfExtent * 2.0f;
    const glm::vec2 gridSnapOrigin(
        std::floor(cameraPosition.x / patchSize) * patchSize,
        std::floor(cameraPosition.z / patchSize) * patchSize);

    m_shader->Use(outputLinear, !outputLinear);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec2("uGridSnapOrigin", gridSnapOrigin);
    m_shader->SetFloat("uGridHeight", m_gridHeight);
    m_shader->SetVec3("uColor", glm::vec3(0.35f, 0.38f, 0.42f));
    m_shader->SetVec3("uCameraPosition", cameraPosition);
    m_shader->SetFloat("uCellSize", m_cellSize);
    m_shader->SetFloat("uMajorInterval", m_majorInterval);
    m_shader->SetFloat("uFadeStart", m_halfExtent * m_fadeStartFraction);
    m_shader->SetFloat("uFadeEnd", m_halfExtent * m_fadeEndFraction);
    m_shader->SetInt("uOutputLinear", outputLinear ? 1 : 0);
    m_shader->SetInt("uSplitLightingOutput", outputLinear ? 1 : 0);
    m_shader->FlushUniforms();

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_vertexBuffer.BindVertex(0, 3 * static_cast<std::uint32_t>(sizeof(float)));
    m_indexBuffer.BindIndex();
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}
