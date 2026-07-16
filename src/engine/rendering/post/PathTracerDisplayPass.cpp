#include "engine/rendering/post/PathTracerDisplayPass.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/platform/FrameDiagnostics.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <cmath>
#include <cstdlib>

namespace
{
    bool ArePathTracerGpuEventsEnabled()
    {
        static const bool enabled = [] {
            const char* const value = std::getenv("GAME_ENGINE_PT_GPU_EVENTS");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    void BeginPathTracerGpuEvent(
        ID3D12GraphicsCommandList* const commandList,
        const wchar_t* const name,
        const UINT nameSize)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->BeginEvent(0, name, nameSize);
        }
    }

    void EndPathTracerGpuEvent(ID3D12GraphicsCommandList* const commandList)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->EndEvent();
        }
    }

    void TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        const D3D12_RESOURCE_STATES before,
        const D3D12_RESOURCE_STATES after)
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

    void CopySrvToInternalHdrTarget(
        const PostProcessContext& context,
        const std::uintptr_t srv,
        PostProcessTarget& target,
        const int width,
        const int height,
        Shader* downsampleShader)
    {
        if (srv == 0 || target.resource == nullptr || width <= 0 || height <= 0
            || downsampleShader == nullptr)
        {
            return;
        }

        const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
        downsampleShader->Use(false, false);
        downsampleShader->BindTextureSlot(0, srv);
        context.draw.DrawFullscreenToTarget(
            *downsampleShader, target, width, height, clearColor, false);
    }

    bool BindGridOverlayDepth(
        const PostProcessContext& context,
        const PathTracerGridOverlayInputs& inputs,
        std::uintptr_t& outDepthDsvCpuHandle)
    {
        if (inputs.sceneFramebuffer == nullptr || !inputs.sceneFramebuffer->IsValid()
            || inputs.sceneFramebuffer->GetDepthResource() == nullptr || inputs.width <= 0
            || inputs.height <= 0)
        {
            return false;
        }

        auto* commandList =
            static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

        if (inputs.renderWidth == inputs.width && inputs.renderHeight == inputs.height)
        {
            if (inputs.sceneFramebuffer->UsesMsaa())
            {
                inputs.sceneFramebuffer->PrepareResolvedDepthForDepthTestPass();
                outDepthDsvCpuHandle = inputs.sceneFramebuffer->GetResolvedDepthDsvCpuHandle();
            }
            else
            {
                inputs.sceneFramebuffer->PrepareDepthForDepthTestPass();
                outDepthDsvCpuHandle = inputs.sceneFramebuffer->GetDepthDsvCpuHandle();
            }
            return outDepthDsvCpuHandle != 0;
        }

        if (inputs.dlssDisplayDepthTarget == nullptr
            || inputs.dlssDisplayDepthTarget->resource == nullptr
            || inputs.dlssDisplayDepthTarget->dsvIndex == UINT32_MAX
            || inputs.dlssDisplayDepthTarget->width != inputs.width
            || inputs.dlssDisplayDepthTarget->height != inputs.height
            || inputs.depthBlitShader == nullptr)
        {
            return false;
        }

        inputs.sceneFramebuffer->EnsureShaderResourceState();

        auto* displayDepth =
            static_cast<ID3D12Resource*>(inputs.dlssDisplayDepthTarget->resource);
        const D3D12_RESOURCE_STATES depthWriteState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        D3D12_RESOURCE_STATES beforeState = static_cast<D3D12_RESOURCE_STATES>(
            inputs.dlssDisplayDepthTarget->resourceState);
        if (beforeState == D3D12_RESOURCE_STATE_COMMON)
        {
            beforeState = depthWriteState;
        }
        TransitionResource(commandList, displayDepth, beforeState, depthWriteState);
        inputs.dlssDisplayDepthTarget->resourceState =
            static_cast<std::uint32_t>(depthWriteState);

        outDepthDsvCpuHandle =
            GfxContext::Get().GetOffscreenDsvCpuHandle(inputs.dlssDisplayDepthTarget->dsvIndex);

        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
        depthDsv.ptr = outDepthDsvCpuHandle;

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(inputs.width);
        viewport.Height = static_cast<float>(inputs.height);
        viewport.MaxDepth = 1.0f;
        const D3D12_RECT scissor{0, 0, inputs.width, inputs.height};

        commandList->OMSetRenderTargets(0, nullptr, FALSE, &depthDsv);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->ClearDepthStencilView(depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        inputs.depthBlitShader->Use(false, false);
        inputs.depthBlitShader->BindTextureSlot(0, inputs.sceneFramebuffer->GetDepthSrvCpuHandle());
        inputs.depthBlitShader->FlushUniforms();
        context.draw.DrawFullscreenQuad();

        commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        return true;
    }
}

bool PathTracerHistoryKey::operator==(const PathTracerHistoryKey& other) const
{
    if (width != other.width || height != other.height
        || convergenceMode != other.convergenceMode
        || geometryObjectCount != other.geometryObjectCount
        || sceneVersion != other.sceneVersion)
    {
        return false;
    }

    if (std::abs(maxTraceDistance - other.maxTraceDistance) > 1e-4f
        || std::abs(sunIntensity - other.sunIntensity) > 1e-4f
        || std::abs(environmentIntensity - other.environmentIntensity) > 1e-4f)
    {
        return false;
    }

    if (glm::length(sunDirection - other.sunDirection) > 1e-5f
        || glm::length(sunColor - other.sunColor) > 1e-5f)
    {
        return false;
    }

    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::abs(viewProjection[column][row] - other.viewProjection[column][row]) > 1e-5f)
            {
                return false;
            }
        }
    }

    return true;
}

void PathTracerDisplayPass::AccumulateReference(
    const PostProcessContext& context,
    const PathTracerAccumulateInputs& inputs,
    PathTracerAccumulateOutputs& outputs)
{
    outputs.historyKey = inputs.currentHistoryKey;
    outputs.sampleCount = inputs.sampleCount;
    outputs.pingPongReadFromScratch = inputs.pingPongReadFromScratch;
    outputs.sumDisplaySrv = 0;

    if (inputs.accumulateShader == nullptr || inputs.currentFrameSrv == 0
        || inputs.width <= 0 || inputs.height <= 0 || inputs.sumTarget == nullptr
        || inputs.scratchTarget == nullptr)
    {
        return;
    }

    const bool historyChanged = !(inputs.historyKey == inputs.currentHistoryKey);
    FrameDiagnostics::LogHistoryEvent(
        0,
        "pt-reference-accumulation",
        historyChanged || inputs.sampleCount == 0 ? "request" : "consume",
        "path-tracer",
        "reference-history-key-v1",
        "none",
        "reference",
        inputs.width,
        inputs.height,
        inputs.width,
        inputs.height,
        false,
        false,
        (historyChanged ? 1u : 0u) | (inputs.sampleCount == 0 ? 2u : 0u));
    if (historyChanged)
    {
        outputs.historyKey = inputs.historyKey;
        outputs.sampleCount = 0;
        outputs.pingPongReadFromScratch = false;
    }

    PostProcessTarget& readTarget =
        outputs.pingPongReadFromScratch ? *inputs.scratchTarget : *inputs.sumTarget;
    PostProcessTarget& writeTarget =
        outputs.pingPongReadFromScratch ? *inputs.sumTarget : *inputs.scratchTarget;

    const bool resetThisFrame = historyChanged || outputs.sampleCount == 0;

    inputs.accumulateShader->Use(false, true);
    inputs.accumulateShader->SetInt("uReset", resetThisFrame ? 1 : 0);
    inputs.accumulateShader->SetInt("uCurrentFrame", 0);
    inputs.accumulateShader->SetInt("uAccumSum", 1);
    inputs.accumulateShader->BindTextureSlot(0, inputs.currentFrameSrv);
    inputs.accumulateShader->BindTextureSlot(1, readTarget.srvCpuHandle);
    inputs.accumulateShader->FlushUniforms();

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto* const commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    static constexpr wchar_t kPathTracerReferenceAccumulationMarker[] = L"PT.ReferenceAccumulation";
    BeginPathTracerGpuEvent(
        commandList,
        kPathTracerReferenceAccumulationMarker,
        static_cast<UINT>(sizeof(kPathTracerReferenceAccumulationMarker)));
    context.draw.DrawFullscreenToTarget(
        *inputs.accumulateShader, writeTarget, inputs.width, inputs.height, clearColor);
    EndPathTracerGpuEvent(commandList);

    outputs.pingPongReadFromScratch = !outputs.pingPongReadFromScratch;
    outputs.sumDisplaySrv = writeTarget.srvCpuHandle;
    ++outputs.sampleCount;
}

void PathTracerDisplayPass::Blit(
    const PostProcessContext& context,
    const PathTracerBlitInputs& inputs)
{
    if (!inputs.pathTracerActive || !inputs.pathTracerBlitReady
        || inputs.pathTracerOutputSrv == 0 || inputs.pathTracerMetadataSrv == 0
        || inputs.outputTarget == nullptr || inputs.primaryDebugShader == nullptr)
    {
        return;
    }

    if (inputs.pathTracerPostIntegrated || inputs.pathTracerDlssResolvedThisFrame)
    {
        return;
    }

    const bool referenceMode = inputs.convergenceMode == PtConvergenceMode::Reference;
    const std::uintptr_t colorSrv =
        referenceMode && inputs.accumSampleCount > 0 && inputs.accumSumDisplaySrv != 0
            ? inputs.accumSumDisplaySrv
            : inputs.pathTracerOutputSrv;
    const int viewMode = referenceMode ? 4 : 3;

    context.draw.BindOutputTarget(
        inputs.outputTarget, inputs.viewportWidth, inputs.viewportHeight);
    inputs.primaryDebugShader->Use(false, true);
    inputs.primaryDebugShader->SetInt("uViewMode", viewMode);
    inputs.primaryDebugShader->SetFloat("uMaxTraceDistance", inputs.maxTraceDistance);
    inputs.primaryDebugShader->SetInt("uSampleCount", static_cast<int>(inputs.accumSampleCount));
    inputs.primaryDebugShader->SetInt("uPrimaryOutput", 0);
    inputs.primaryDebugShader->SetInt("uPrimaryMetadata", 1);
    inputs.primaryDebugShader->BindTextureSlot(0, colorSrv);
    inputs.primaryDebugShader->BindTextureSlot(1, inputs.pathTracerMetadataSrv);
    inputs.primaryDebugShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();
}

void PathTracerDisplayPass::CopyHdrToCompositeTarget(
    const PostProcessContext& context,
    const PathTracerHdrCopyInputs& inputs,
    const float clearColor[4])
{
    if (inputs.hdrCompositeTarget == nullptr || context.renderWidth <= 0
        || context.renderHeight <= 0)
    {
        return;
    }

    const bool referenceMode = inputs.convergenceMode == PtConvergenceMode::Reference;

    if (referenceMode && inputs.accumSampleCount > 0 && inputs.accumSumDisplaySrv != 0
        && inputs.meanShader != nullptr)
    {
        inputs.meanShader->Use(false, true);
        inputs.meanShader->SetInt("uSampleCount", static_cast<int>(inputs.accumSampleCount));
        inputs.meanShader->SetInt("uAccumSum", 0);
        inputs.meanShader->BindTextureSlot(0, inputs.accumSumDisplaySrv);
        inputs.meanShader->FlushUniforms();
        context.draw.DrawFullscreenToTarget(
            *inputs.meanShader,
            *inputs.hdrCompositeTarget,
            context.renderWidth,
            context.renderHeight,
            clearColor);
        return;
    }

    CopySrvToInternalHdrTarget(
        context,
        inputs.pathTracerOutputSrv,
        *inputs.hdrCompositeTarget,
        context.renderWidth,
        context.renderHeight,
        inputs.downsampleShader);
}

void PathTracerDisplayPass::PrepareDlssHdrInput(
    const PostProcessContext& context,
    const PathTracerHdrCopyInputs& inputs)
{
    if (inputs.pathTracerOutputSrv == 0 || context.renderWidth <= 0 || context.renderHeight <= 0
        || inputs.hdrCompositeTarget == nullptr)
    {
        return;
    }

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    CopyHdrToCompositeTarget(context, inputs, clearColor);
}

void PathTracerDisplayPass::IntegrateIntoHdrChain(
    const PostProcessContext& context,
    const PathTracerIntegrateInputs& inputs,
    PathTracerIntegrateOutputs& outputs)
{
    outputs.integrated = false;
    outputs.hdrColorSrv = 0;

    if (!inputs.pathTracerActive || inputs.pathTracerOutputSrv == 0
        || inputs.hdrCopy.hdrCompositeTarget == nullptr)
    {
        return;
    }

    const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    CopyHdrToCompositeTarget(context, inputs.hdrCopy, compositeClear);

    if (inputs.gridOverlayEnabled && inputs.drawGridOverlay && inputs.camera != nullptr)
    {
        inputs.drawGridOverlay(
            *inputs.hdrCopy.hdrCompositeTarget,
            context.renderWidth,
            context.renderHeight);
    }

    outputs.hdrColorSrv = inputs.hdrCopy.hdrCompositeTarget->srvCpuHandle;
    outputs.integrated = true;
}

void PathTracerDisplayPass::DrawGridOverlay(
    const PostProcessContext& context,
    const PathTracerGridOverlayInputs& inputs)
{
    if (inputs.camera == nullptr || inputs.target == nullptr
        || inputs.target->resource == nullptr || inputs.target->rtvIndex == UINT32_MAX
        || inputs.width <= 0 || inputs.height <= 0 || !inputs.gridOverlayDraw)
    {
        return;
    }

    auto* commandList =
        static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* resource = static_cast<ID3D12Resource*>(inputs.target->resource);

    const D3D12_RESOURCE_STATES renderTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES beforeState =
        static_cast<D3D12_RESOURCE_STATES>(inputs.target->resourceState);
    if (beforeState == D3D12_RESOURCE_STATE_COMMON)
    {
        beforeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    TransitionResource(commandList, resource, beforeState, renderTargetState);
    inputs.target->resourceState = static_cast<std::uint32_t>(renderTargetState);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{
        GfxContext::Get().GetOffscreenRtvCpuHandle(inputs.target->rtvIndex)};

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    const D3D12_CPU_DESCRIPTOR_HANDLE* depthPtr = nullptr;
    std::uintptr_t depthDsvCpuHandle = 0;
    const bool useDepthTest = BindGridOverlayDepth(context, inputs, depthDsvCpuHandle);
    if (useDepthTest)
    {
        depthDsv.ptr = depthDsvCpuHandle;
        depthPtr = &depthDsv;
    }

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(inputs.width);
    viewport.Height = static_cast<float>(inputs.height);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, inputs.width, inputs.height};

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, depthPtr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    inputs.gridOverlayDraw(*inputs.camera, useDepthTest);

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    TransitionResource(
        commandList,
        resource,
        renderTargetState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    inputs.target->resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    GfxContext::Get().RebindFrameDescriptorHeaps();
}

bool PathTracerDisplayPass::ResolveDlssDepth(
    const PostProcessContext& context,
    const PathTracerDlssDepthResolveInputs& inputs)
{
    if (inputs.pathTracerDepthSrv == 0 || inputs.ptDlssDepthTarget == nullptr
        || inputs.ptDlssDepthTarget->resource == nullptr
        || inputs.ptDlssDepthTarget->dsvIndex == UINT32_MAX
        || inputs.ptDlssDepthTarget->width != inputs.renderWidth
        || inputs.ptDlssDepthTarget->height != inputs.renderHeight
        || inputs.renderWidth <= 0 || inputs.renderHeight <= 0
        || inputs.depthBlitShader == nullptr)
    {
        return false;
    }

    auto* commandList =
        static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    auto* d24Depth = static_cast<ID3D12Resource*>(inputs.ptDlssDepthTarget->resource);
    const D3D12_RESOURCE_STATES depthWriteState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES beforeState = static_cast<D3D12_RESOURCE_STATES>(
        inputs.ptDlssDepthTarget->resourceState);
    if (beforeState == D3D12_RESOURCE_STATE_COMMON)
    {
        beforeState = depthWriteState;
    }
    if (beforeState != depthWriteState)
    {
        TransitionResource(commandList, d24Depth, beforeState, depthWriteState);
    }
    inputs.ptDlssDepthTarget->resourceState =
        static_cast<std::uint32_t>(depthWriteState);

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    depthDsv.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(inputs.ptDlssDepthTarget->dsvIndex);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(inputs.renderWidth);
    viewport.Height = static_cast<float>(inputs.renderHeight);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, inputs.renderWidth, inputs.renderHeight};

    commandList->OMSetRenderTargets(0, nullptr, FALSE, &depthDsv);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearDepthStencilView(depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    inputs.depthBlitShader->Use(false, false);
    inputs.depthBlitShader->BindTextureSlot(0, inputs.pathTracerDepthSrv);
    inputs.depthBlitShader->FlushUniforms();
    context.draw.DrawFullscreenQuad();

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    TransitionResource(
        commandList, d24Depth, depthWriteState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    inputs.ptDlssDepthTarget->resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool PathTracerDisplayPass::PatchSkyMotion(
    const PostProcessContext& context,
    const PathTracerSkyMotionPatchInputs& inputs)
{
    if (inputs.pathTracerMetadataSrv == 0 || inputs.pathTracerMotionSrv == 0
        || inputs.sceneFramebuffer == nullptr || !inputs.sceneFramebuffer->HasVelocity()
        || context.renderWidth <= 0 || context.renderHeight <= 0
        || inputs.ptDlssMotionTarget == nullptr
        || inputs.ptDlssMotionTarget->resource == nullptr
        || inputs.ptDlssMotionTarget->rtvIndex == UINT32_MAX
        || inputs.ptDlssMotionTarget->width != context.renderWidth
        || inputs.ptDlssMotionTarget->height != context.renderHeight
        || inputs.skyMotionPatchShader == nullptr)
    {
        return false;
    }

    inputs.sceneFramebuffer->EnsureShaderResourceState();

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
    inputs.skyMotionPatchShader->Use(false, false);
    inputs.skyMotionPatchShader->BindTextureSlot(
        0,
        inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity));
    inputs.skyMotionPatchShader->BindTextureSlot(1, inputs.pathTracerMotionSrv);
    inputs.skyMotionPatchShader->BindTextureSlot(2, inputs.pathTracerMetadataSrv);
    context.draw.DrawFullscreenToTarget(
        *inputs.skyMotionPatchShader,
        *inputs.ptDlssMotionTarget,
        context.renderWidth,
        context.renderHeight,
        clearColor);
    return true;
}
