#include "engine/gizmos/SelectionRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <stdexcept>

namespace
{
    constexpr float kOutlineWidthPixels = 2.0f;
    constexpr float kOutlineWidthWorld = 0.004f;
    constexpr float kRadialExpand = 0.012f;
    constexpr float kGlowBlurRadius = 2.0f;
    constexpr float kGlowIntensity = 1.1f;
    constexpr glm::vec3 kSelectionColor(1.0f, 0.85f, 0.22f);
    constexpr glm::vec3 kSelectionGlowColor(1.15f, 0.95f, 0.35f);

    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };

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

    void CreateTexture2DSrv(
        ID3D12Resource* resource,
        const std::uint32_t descriptorIndex,
        DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        GfxContext::Get().CreateShaderResourceView(resource, &srvDesc, descriptorIndex);
    }

    void DrawMeshesMask(
        const Shader& shader,
        const Camera& camera,
        const std::vector<SelectionMeshDraw>& meshes)
    {
        shader.SetMat4("uView", camera.GetViewMatrix());
        shader.SetMat4("uProjection", camera.GetUnjitteredProjectionMatrix());

        for (const SelectionMeshDraw& meshDraw : meshes)
        {
            if (meshDraw.mesh == nullptr)
            {
                continue;
            }

            shader.SetMat4("uModel", meshDraw.worldMatrix);
            shader.FlushUniforms();
            meshDraw.mesh->Draw();
        }
    }
}

SelectionRenderer::SelectionRenderer()
    : m_maskShader(std::make_unique<Shader>(
          EngineConstants::SelectionMaskVertexShader,
          EngineConstants::SelectionMaskFragmentShader)),
      m_edgeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionEdgeFragmentShader)),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomBlurFragmentShader)),
      m_glowShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionGlowFragmentShader)),
      m_sharpShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SelectionSharpFragmentShader)),
      m_hullShader(std::make_unique<Shader>(
          EngineConstants::SelectionOutlineVertexShader,
          EngineConstants::SelectionOutlineFragmentShader))
{
    CreateFullscreenQuad();
}

SelectionRenderer::~SelectionRenderer()
{
    DestroyTargets();
}

void SelectionRenderer::CreateFullscreenQuad()
{
    m_quadVb.Create(
        GpuBuffer::Type::Vertex,
        kQuadVertices,
        static_cast<std::uint32_t>(sizeof(kQuadVertices)));
}

bool SelectionRenderer::CreateInternalTarget(InternalTarget& target, const int width, const int height) const
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    const DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = format;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        return false;
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (target.srvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        return false;
    }

    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);
    target.rtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);
    if (target.rtvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        return false;
    }

    CreateTexture2DSrv(resource, target.srvIndex, format);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};
    device->CreateRenderTargetView(resource, nullptr, rtvHandle);
    return true;
}

void SelectionRenderer::DestroyInternalTarget(InternalTarget& target) const
{
    if (target.srvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(target.srvIndex);
        target.srvIndex = UINT32_MAX;
    }

    if (target.rtvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenRtvBlock(target.rtvIndex, 1);
        target.rtvIndex = UINT32_MAX;
    }

    if (target.allocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(target.allocation)->Release();
        target.allocation = nullptr;
    }

    target.resource = nullptr;
    target.srvCpuHandle = 0;
    target.width = 0;
    target.height = 0;
}

void SelectionRenderer::DestroyTargets() const
{
    DestroyInternalTarget(m_maskTarget);
    DestroyInternalTarget(m_edgeTarget);
    DestroyInternalTarget(m_glowBlurTarget);
    DestroyInternalTarget(m_glowBlur2Target);
    m_targetWidth = 0;
    m_targetHeight = 0;
}

void SelectionRenderer::ResizeTargets(const int width, const int height) const
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (m_targetWidth == width && m_targetHeight == height && m_maskTarget.resource != nullptr)
    {
        return;
    }

    DestroyTargets();

    const int blurWidth = std::max(1, width / 2);
    const int blurHeight = std::max(1, height / 2);
    if (!CreateInternalTarget(m_maskTarget, width, height) ||
        !CreateInternalTarget(m_edgeTarget, width, height) ||
        !CreateInternalTarget(m_glowBlurTarget, blurWidth, blurHeight) ||
        !CreateInternalTarget(m_glowBlur2Target, blurWidth, blurHeight))
    {
        DestroyTargets();
    }

    m_targetWidth = width;
    m_targetHeight = height;
}

void SelectionRenderer::DrawFullscreenQuad() const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_quadVb.BindVertex(0, 4 * static_cast<std::uint32_t>(sizeof(float)));
    commandList->DrawInstanced(6, 1, 0, 0);
}

void SelectionRenderer::DrawFullscreenToTarget(
    Shader& shader,
    InternalTarget& target,
    const int width,
    const int height,
    const float clearColor[4]) const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* resource = static_cast<ID3D12Resource*>(target.resource);

    TransitionResource(
        commandList,
        resource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    shader.BindPipeline();
    shader.FlushUniforms();
    DrawFullscreenQuad();

    TransitionResource(
        commandList,
        resource,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void SelectionRenderer::DrawScreenSpace(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    const int width,
    const int height) const
{
    const Framebuffer* outputTarget = GfxContext::Get().GetBoundOutputFramebuffer();

    ResizeTargets(width, height);
    if (m_maskTarget.resource == nullptr || m_edgeTarget.resource == nullptr)
    {
        return;
    }

    const int blurWidth = std::max(1, width / 2);
    const int blurHeight = std::max(1, height / 2);
    const float blurTexelSizeX = 1.0f / static_cast<float>(blurWidth);
    const float blurTexelSizeY = 1.0f / static_cast<float>(blurHeight);
    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};

    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        auto* maskResource = static_cast<ID3D12Resource*>(m_maskTarget.resource);
        TransitionResource(
            commandList,
            maskResource,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(m_maskTarget.rtvIndex)};
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, width, height};
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        m_maskShader->Use();
        DrawMeshesMask(*m_maskShader, camera, meshes);

        TransitionResource(
            commandList,
            maskResource,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_edgeShader->SetInt("uMask", 0);
    m_edgeShader->SetVec2(
        "uTexelSize",
        glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)));
    m_edgeShader->SetFloat("uOutlineWidth", kOutlineWidthPixels);
    m_edgeShader->BindTextureSlot(0, m_maskTarget.srvCpuHandle);
    DrawFullscreenToTarget(*m_edgeShader, const_cast<InternalTarget&>(m_edgeTarget), width, height, clearColor);

    m_blurShader->SetInt("uInput", 0);
    m_blurShader->SetFloat("uDirectionX", blurTexelSizeX);
    m_blurShader->SetFloat("uDirectionY", 0.0f);
    m_blurShader->SetFloat("uBlurRadius", kGlowBlurRadius);
    m_blurShader->BindTextureSlot(0, m_edgeTarget.srvCpuHandle);
    DrawFullscreenToTarget(
        *m_blurShader,
        const_cast<InternalTarget&>(m_glowBlurTarget),
        blurWidth,
        blurHeight,
        clearColor);

    m_blurShader->SetInt("uInput", 0);
    m_blurShader->SetFloat("uDirectionX", 0.0f);
    m_blurShader->SetFloat("uDirectionY", blurTexelSizeY);
    m_blurShader->SetFloat("uBlurRadius", kGlowBlurRadius);
    m_blurShader->BindTextureSlot(0, m_glowBlurTarget.srvCpuHandle);
    DrawFullscreenToTarget(
        *m_blurShader,
        const_cast<InternalTarget&>(m_glowBlur2Target),
        blurWidth,
        blurHeight,
        clearColor);

    if (outputTarget != nullptr)
    {
        outputTarget->BindDrawTarget(false);
    }
    else
    {
        GfxContext::Get().BindSwapChainRenderTarget(false);
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    m_glowShader->Use(false, true);
    m_glowShader->SetInt("uGlow", 0);
    m_glowShader->SetInt("uEdge", 1);
    m_glowShader->SetVec3("uColor", kSelectionGlowColor);
    m_glowShader->SetFloat("uGlowIntensity", kGlowIntensity);
    m_glowShader->BindTextureSlot(0, m_glowBlur2Target.srvCpuHandle);
    m_glowShader->BindTextureSlot(1, m_edgeTarget.srvCpuHandle);
    m_glowShader->FlushUniforms();
    DrawFullscreenQuad();

    m_sharpShader->Use(false, true);
    m_sharpShader->SetInt("uEdge", 0);
    m_sharpShader->SetVec3("uColor", kSelectionColor);
    m_sharpShader->BindTextureSlot(0, m_edgeTarget.srvCpuHandle);
    m_sharpShader->FlushUniforms();
    DrawFullscreenQuad();
}

void SelectionRenderer::DrawHullFallback(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    const int width,
    const int height) const
{
    DrawScreenSpace(camera, meshes, width, height);
}

void SelectionRenderer::Draw(
    const Camera& camera,
    const std::vector<SelectionMeshDraw>& meshes,
    const bool useScreenSpace) const
{
    if (meshes.empty())
    {
        return;
    }

    const int width = GfxContext::Get().GetWidth();
    const int height = GfxContext::Get().GetHeight();
    int renderWidth = width;
    int renderHeight = height;
    GfxContext::Get().GetOutputRenderSize(renderWidth, renderHeight);

    if (renderWidth <= 0 || renderHeight <= 0)
    {
        return;
    }

    if (useScreenSpace)
    {
        DrawScreenSpace(camera, meshes, renderWidth, renderHeight);
    }
    else
    {
        DrawHullFallback(camera, meshes, renderWidth, renderHeight);
    }
}
