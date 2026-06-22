#pragma once

#include <memory>
#include <vector>

class Camera;
class Shader;

struct SelectionMeshDraw;

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
};
