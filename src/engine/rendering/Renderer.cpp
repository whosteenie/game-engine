#include "engine/rendering/Renderer.h"

#include "engine/rhi/GfxContext.h"

void Renderer::SetViewport(int width, int height)
{
    GfxContext::Get().Resize(width, height);
}

void Renderer::BeginFrame() const
{
    GfxContext::Get().BeginFrame();
}

void Renderer::CancelFrame() const
{
    GfxContext::Get().CancelFrame();
}

void Renderer::EndFrame(GLFWwindow* /*window*/) const
{
    GfxContext::Get().EndFrame();
}
