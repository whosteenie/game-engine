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
    void ResizeMaskTarget(int width, int height) const;
    void DestroyMaskTarget() const;
    void CreateMaskTarget(int width, int height) const;
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
    std::unique_ptr<Shader> m_hullShader;

    mutable unsigned int m_maskFbo = 0;
    mutable unsigned int m_maskTexture = 0;
    mutable int m_maskWidth = 0;
    mutable int m_maskHeight = 0;

    unsigned int m_quadVao = 0;
    unsigned int m_quadVbo = 0;
};
