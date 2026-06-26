#pragma once

#include <memory>

#include "engine/rhi/d3d12/GpuBuffer.h"

class Camera;
class Shader;

class GridRenderer
{
public:
    GridRenderer();
    ~GridRenderer();

    GridRenderer(const GridRenderer&) = delete;
    GridRenderer& operator=(const GridRenderer&) = delete;
    GridRenderer(GridRenderer&& other) noexcept;
    GridRenderer& operator=(GridRenderer&& other) noexcept;

    void Draw(const Camera& camera, bool outputLinear = false) const;

private:
    void BuildGridGeometry(float halfExtent);
    void ReleaseGpuResources();

    std::unique_ptr<Shader> m_shader;
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
    unsigned int m_indexCount = 0;

    float m_halfExtent = 256.0f;
    float m_cellSize = 1.0f;
    float m_majorInterval = 10.0f;
    float m_gridHeight = 0.051f;
    float m_fadeStartFraction = 0.35f;
    float m_fadeEndFraction = 0.92f;
};
