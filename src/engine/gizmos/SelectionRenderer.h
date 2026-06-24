#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/scene/SceneHierarchy.h"

#if defined(GAME_ENGINE_D3D12)
#include "engine/rhi/d3d12/GpuBuffer.h"
#endif

class Camera;
class Shader;

class SelectionRenderer
{
public:
    SelectionRenderer();
    ~SelectionRenderer();

    void Draw(const Camera& camera, const std::vector<SelectionMeshDraw>& meshes, bool useScreenSpace) const;

private:
    void ResizeTargets(int width, int height) const;
    void DestroyTargets() const;
    void CreateColorTarget(unsigned int& fbo, unsigned int& texture, int width, int height) const;
    void DestroyColorTarget(unsigned int& fbo, unsigned int& texture) const;
    void CreateFullscreenQuad();
    void DrawFullscreenQuad() const;

    void DrawScreenSpace(
        const Camera& camera,
        const std::vector<SelectionMeshDraw>& meshes,
        int width,
        int height) const;

    void DrawHullFallback(
        const Camera& camera,
        const std::vector<SelectionMeshDraw>& meshes,
        int width,
        int height) const;

    std::unique_ptr<Shader> m_maskShader;
    std::unique_ptr<Shader> m_edgeShader;
    std::unique_ptr<Shader> m_blurShader;
    std::unique_ptr<Shader> m_glowShader;
    std::unique_ptr<Shader> m_sharpShader;
    std::unique_ptr<Shader> m_hullShader;

#if defined(GAME_ENGINE_D3D12)
    struct InternalTarget
    {
        void* resource = nullptr;
        void* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uintptr_t srvCpuHandle = 0;
        std::uint32_t rtvIndex = UINT32_MAX;
        int width = 0;
        int height = 0;
    };

    void DrawFullscreenToTarget(
        Shader& shader,
        InternalTarget& target,
        int width,
        int height,
        const float clearColor[4]) const;
    bool CreateInternalTarget(InternalTarget& target, int width, int height) const;
    void DestroyInternalTarget(InternalTarget& target) const;

    mutable InternalTarget m_maskTarget;
    mutable InternalTarget m_edgeTarget;
    mutable InternalTarget m_glowBlurTarget;
    mutable InternalTarget m_glowBlur2Target;
    mutable int m_targetWidth = 0;
    mutable int m_targetHeight = 0;

    GpuBuffer m_quadVb;
#else
    mutable unsigned int m_maskFbo = 0;
    mutable unsigned int m_maskTexture = 0;
    mutable unsigned int m_edgeFbo = 0;
    mutable unsigned int m_edgeTexture = 0;
    mutable unsigned int m_glowBlurFbo = 0;
    mutable unsigned int m_glowBlurTexture = 0;
    mutable unsigned int m_glowBlur2Fbo = 0;
    mutable unsigned int m_glowBlur2Texture = 0;
    mutable int m_targetWidth = 0;
    mutable int m_targetHeight = 0;

    unsigned int m_quadVao = 0;
    unsigned int m_quadVbo = 0;
#endif
};
