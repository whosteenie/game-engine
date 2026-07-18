#pragma once

#include "engine/rhi/d3d12/GpuBuffer.h"

#include <memory>

class Camera;
class IBL;
class Shader;

class SkyboxRenderer
{
public:
    SkyboxRenderer();
    ~SkyboxRenderer();

    SkyboxRenderer(const SkyboxRenderer&) = delete;
    SkyboxRenderer& operator=(const SkyboxRenderer&) = delete;
    SkyboxRenderer(SkyboxRenderer&& other) noexcept;
    SkyboxRenderer& operator=(SkyboxRenderer&& other) noexcept;

    void Draw(const Camera& camera, const IBL& environment, float exposure, bool splitLightingMrt) const;

private:
    std::unique_ptr<Shader> m_shader;
    GpuBuffer m_quadVertexBuffer;
};
