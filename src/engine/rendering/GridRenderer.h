#pragma once

#include <memory>

#if defined(GAME_ENGINE_D3D12)
#include "engine/rhi/d3d12/GpuBuffer.h"
#endif

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
    void DestroyGlResources();

    std::unique_ptr<Shader> m_shader;
#if !defined(GAME_ENGINE_D3D12)
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
#else
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
#endif
    unsigned int m_indexCount = 0;

    float m_halfExtent = 14.0f;
    float m_cellSize = 1.0f;
    float m_majorInterval = 10.0f;
};
