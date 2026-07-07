#include "engine/rendering/GridRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <vector>

namespace
{
    struct GridLodSettings
    {
        float drawHalfExtent = 80.0f;
        float fadeStart = 28.0f;
        float fadeEnd = 72.0f;
        float maxRenderDistance = 76.0f;
    };

    GridLodSettings ComputeGridLod(const Camera& camera, const float maxDrawHalfExtent)
    {
        const glm::vec3 cameraPosition = camera.GetPosition();
        const float cameraY = glm::max(cameraPosition.y, 0.5f);
        const float fovRadians = glm::radians(camera.GetFov());
        const float tanHalfVertFov = glm::max(std::tan(fovRadians * 0.5f), 0.01f);

        const glm::vec3 front = camera.GetFront();
        const float horizontalView = glm::clamp(glm::length(glm::vec2(front.x, front.z)), 0.0f, 1.0f);

        // Screen-bottom ground reach plus extra when the view grazes the horizon.
        const float screenGroundReach =
            cameraY / glm::max(tanHalfVertFov * 0.26f, 0.04f);
        const float horizonReach =
            cameraY / glm::max(std::abs(front.y), 0.025f) * 0.36f;
        float visibleRange = glm::max(screenGroundReach, horizonReach);
        visibleRange *= glm::mix(0.92f, 1.42f, horizontalView);
        visibleRange = glm::clamp(visibleRange, 88.0f, maxDrawHalfExtent);

        const float fadeStart = visibleRange * 0.60f;
        const float fadeEnd = visibleRange * 0.95f;
        const float drawHalfExtent = glm::min(visibleRange, maxDrawHalfExtent);
        const float maxRenderDistance = drawHalfExtent * 0.98f;

        return GridLodSettings{
            drawHalfExtent,
            fadeStart,
            fadeEnd,
            maxRenderDistance,
        };
    }
}

GridRenderer::GridRenderer()
    : m_shader(std::make_unique<Shader>(EngineConstants::GridVertexShader, EngineConstants::GridFragmentShader))
{
    BuildGridGeometry();
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
      m_maxDrawHalfExtent(other.m_maxDrawHalfExtent),
      m_cellSize(other.m_cellSize),
      m_majorInterval(other.m_majorInterval),
      m_gridHeight(other.m_gridHeight)
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
        m_maxDrawHalfExtent = other.m_maxDrawHalfExtent;
        m_cellSize = other.m_cellSize;
        m_majorInterval = other.m_majorInterval;
        m_gridHeight = other.m_gridHeight;
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

void GridRenderer::BuildGridGeometry()
{
    // Unit quad; world extent is scaled per frame from camera LOD.
    const std::vector<float> vertices = {
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
         1.0f, 0.0f,  1.0f,
        -1.0f, 0.0f,  1.0f,
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

void GridRenderer::Draw(
    const Camera& camera,
    const bool outputLinear,
    const bool useUnjitteredProjection,
    const bool useSplitLightingMrt,
    const bool useDepthTest) const
{
    if (!m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid() || m_indexCount == 0)
    {
        return;
    }

    const glm::vec3 cameraPosition = camera.GetPosition();
    const glm::vec2 gridSnapOrigin(
        std::floor(cameraPosition.x / m_cellSize) * m_cellSize,
        std::floor(cameraPosition.z / m_cellSize) * m_cellSize);

    const GridLodSettings lod = ComputeGridLod(camera, m_maxDrawHalfExtent);

    const float heightBoost = glm::clamp(cameraPosition.y * 0.0015f, 0.0f, 2.0f);
    const float effectiveGridHeight = m_gridHeight + heightBoost;

    const float altitudeUlp = glm::clamp((cameraPosition.y - 8.0f) / 40.0f, 0.0f, 48.0f);
    const float clipDepthBiasUlp = 4.0f + altitudeUlp;

    const bool mrtPass = outputLinear && useSplitLightingMrt;
    m_shader->Use(mrtPass, !outputLinear, false, false, !useDepthTest);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4(
        "uProjection",
        useUnjitteredProjection ? camera.GetUnjitteredProjectionMatrix() : camera.GetProjectionMatrix());
    m_shader->SetVec2("uGridSnapOrigin", gridSnapOrigin);
    m_shader->SetFloat("uGridHeight", effectiveGridHeight);
    m_shader->SetFloat("uDrawHalfExtent", lod.drawHalfExtent);
    m_shader->SetFloat("uClipDepthBiasUlp", clipDepthBiasUlp);
    m_shader->SetVec3("uColor", glm::vec3(0.35f, 0.38f, 0.42f));
    m_shader->SetVec3("uCameraPosition", cameraPosition);
    m_shader->SetFloat("uCellSize", m_cellSize);
    m_shader->SetFloat("uMajorInterval", m_majorInterval);
    m_shader->SetFloat("uFadeStart", lod.fadeStart);
    m_shader->SetFloat("uFadeEnd", lod.fadeEnd);
    m_shader->SetFloat("uMaxRenderDistance", lod.maxRenderDistance);
    m_shader->SetInt("uOutputLinear", outputLinear ? 1 : 0);
    m_shader->SetInt("uSplitLightingOutput", useSplitLightingMrt && outputLinear ? 1 : 0);
    m_shader->FlushUniforms();

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_vertexBuffer.BindVertex(0, 3 * static_cast<std::uint32_t>(sizeof(float)));
    m_indexBuffer.BindIndex();
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}
