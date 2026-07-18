#pragma once

#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/post/PostProcessTarget.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

class Shader;

// Fullscreen-quad draw helpers shared by post-process passes (HK-C0).
class PostProcessDraw
{
public:
    explicit PostProcessDraw(GpuBuffer& quadVb);

    void DrawFullscreenQuad() const;
    void DrawFullscreenPass(Shader& shader, bool viewportLdr) const;
    void DrawFullscreenToTarget(
        Shader& shader,
        PostProcessTarget& target,
        int width,
        int height,
        const float clearColor[4],
        bool viewportLdr = false) const;
    void BindOutputTarget(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;

private:
    GpuBuffer& m_quadVb;
};
