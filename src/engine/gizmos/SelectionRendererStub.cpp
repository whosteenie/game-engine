#include "engine/gizmos/SelectionRenderer.h"

#include "engine/rendering/Shader.h"

SelectionRenderer::SelectionRenderer() = default;

SelectionRenderer::~SelectionRenderer() = default;

void SelectionRenderer::Draw(
    const Camera& /*camera*/,
    const std::vector<SelectionMeshDraw>& /*meshes*/,
    bool /*useScreenSpace*/) const
{
}

void SelectionRenderer::ResizeTargets(int /*width*/, int /*height*/) const
{
}

void SelectionRenderer::DestroyTargets() const
{
}

void SelectionRenderer::CreateColorTarget(
    unsigned int& /*fbo*/,
    unsigned int& /*texture*/,
    int /*width*/,
    int /*height*/) const
{
}

void SelectionRenderer::DestroyColorTarget(unsigned int& /*fbo*/, unsigned int& /*texture*/) const
{
}

void SelectionRenderer::CreateFullscreenQuad()
{
}

void SelectionRenderer::DrawFullscreenQuad() const
{
}

void SelectionRenderer::DrawScreenSpace(
    const Camera& /*camera*/,
    const std::vector<SelectionMeshDraw>& /*meshes*/,
    int /*width*/,
    int /*height*/) const
{
}

void SelectionRenderer::DrawHullFallback(
    const Camera& /*camera*/,
    const std::vector<SelectionMeshDraw>& /*meshes*/,
    int /*width*/,
    int /*height*/) const
{
}
