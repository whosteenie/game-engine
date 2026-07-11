#include "engine/rendering/post/PostProcessDraw.h"

#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>
#include <dxgiformat.h>

namespace
{
    bool IsLdrRenderTargetFormat(const int format)
    {
        return format == static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM)
            || format == static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    }

    bool IsSingleChannelRenderTargetFormat(const int format)
    {
        return format == static_cast<int>(DXGI_FORMAT_R16_FLOAT);
    }

    bool IsHdr32RenderTargetFormat(const int format)
    {
        return format == static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT);
    }

    void ResolveFullscreenPipelineFlags(
        const int targetFormat,
        const bool viewportLdrOverride,
        bool& outViewportLdr,
        bool& outSingleChannelRtv,
        bool& outHdr32Rtv)
    {
        outViewportLdr = viewportLdrOverride || IsLdrRenderTargetFormat(targetFormat);
        outSingleChannelRtv =
            !outViewportLdr && IsSingleChannelRenderTargetFormat(targetFormat);
        outHdr32Rtv =
            !outViewportLdr && !outSingleChannelRtv && IsHdr32RenderTargetFormat(targetFormat);
    }

    void TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        if (before == after || resource == nullptr)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
    }
}

PostProcessDraw::PostProcessDraw(GpuBuffer& quadVb)
    : m_quadVb(quadVb)
{
}

void PostProcessDraw::DrawFullscreenQuad() const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_quadVb.BindVertex(0, 4 * static_cast<std::uint32_t>(sizeof(float)));
    commandList->DrawInstanced(6, 1, 0, 0);
}

void PostProcessDraw::DrawFullscreenPass(Shader& shader, const bool viewportLdr) const
{
    shader.BindPipeline(false, viewportLdr, false, false, false, false);
    shader.FlushUniforms();
    DrawFullscreenQuad();
}

void PostProcessDraw::DrawFullscreenToTarget(
    Shader& shader,
    PostProcessTarget& target,
    const int width,
    const int height,
    const float clearColor[4],
    const bool viewportLdr) const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* resource = static_cast<ID3D12Resource*>(target.resource);

    const D3D12_RESOURCE_STATES renderTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_STATES shaderResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES beforeState = static_cast<D3D12_RESOURCE_STATES>(target.resourceState);
    if (beforeState == D3D12_RESOURCE_STATE_COMMON)
    {
        beforeState = shaderResourceState;
    }
    TransitionResource(commandList, resource, beforeState, renderTargetState);
    target.resourceState = static_cast<std::uint32_t>(renderTargetState);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{
        GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    bool useLdrPipeline = viewportLdr;
    bool useSingleChannelPipeline = false;
    bool useHdr32Pipeline = false;
    ResolveFullscreenPipelineFlags(
        target.format, viewportLdr, useLdrPipeline, useSingleChannelPipeline, useHdr32Pipeline);
    shader.BindPipeline(
        false, useLdrPipeline, false, false, false, useSingleChannelPipeline, useHdr32Pipeline);
    shader.FlushUniforms();
    DrawFullscreenQuad();

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    TransitionResource(commandList, resource, renderTargetState, shaderResourceState);
    target.resourceState = static_cast<std::uint32_t>(shaderResourceState);
}

void PostProcessDraw::BindOutputTarget(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    int outputWidth = viewportWidth;
    int outputHeight = viewportHeight;

    if (outputTarget != nullptr)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(outputTarget);
        outputTarget->BindDrawTarget(false);
        if (outputTarget->GetWidth() > 0 && outputTarget->GetHeight() > 0)
        {
            outputWidth = outputTarget->GetWidth();
            outputHeight = outputTarget->GetHeight();
        }
    }
    else
    {
        GfxContext::Get().BindSwapChainRenderTarget(false);
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(outputWidth);
    viewport.Height = static_cast<float>(outputHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, outputWidth, outputHeight};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
}
