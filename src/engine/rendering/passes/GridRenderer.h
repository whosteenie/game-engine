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

    // outputLinear: HDR (linear) vs sRGB/LDR target.
    // useUnjitteredProjection: viewport overlays after TAA/DLSS use the resolved projection.
    // useSplitLightingMrt: scene-pass grid writes split-lighting MRT (4 RTVs). Single-RT HDR
    // overlays (DLSS output / PT composite) must pass false or the MRT PSO won't match.
    // useDepthTest: false when no scene DSV is bound (DLSS display-res HDR overlay).
    void Draw(
        const Camera& camera,
        bool outputLinear = false,
        bool useUnjitteredProjection = false,
        bool useSplitLightingMrt = true,
        bool useDepthTest = true) const;

private:
    void BuildGridGeometry();
    void ReleaseGpuResources();

    std::unique_ptr<Shader> m_shader;
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
    unsigned int m_indexCount = 0;

    float m_maxDrawHalfExtent = 640.0f;
    float m_cellSize = 1.0f;
    float m_majorInterval = 10.0f;
    float m_gridHeight = 0.051f;
};
