#include "engine/rendering/post/DlssResolvePass.h"

#include "engine/camera/Camera.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
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

    glm::vec2 JitterNdcToPixels(const glm::vec2& jitterNdc, const int width, const int height)
    {
        return glm::vec2(
            jitterNdc.x * 0.5f * static_cast<float>(width),
            -jitterNdc.y * 0.5f * static_cast<float>(height));
    }

    glm::vec2 DlssMvecScale()
    {
        return glm::vec2(-0.5f, 0.5f);
    }

    float DlssExposureScaleFromEv(const float exposureEv)
    {
        return std::exp2(exposureEv);
    }

    void LogRrExposureDiagnosticsOnChange(
        const float exposureEv,
        const float exposureScale,
        const float meanInputLuminance,
        const bool meanInputLuminanceValid)
    {
        static float lastExposureEv = std::numeric_limits<float>::quiet_NaN();
        static bool lastMeanValid = false;
        if (exposureEv == lastExposureEv && meanInputLuminanceValid == lastMeanValid)
        {
            return;
        }

        lastExposureEv = exposureEv;
        lastMeanValid = meanInputLuminanceValid;
        std::ostringstream message;
        message << std::fixed << std::setprecision(5)
                << "tonemap exposure EV=" << exposureEv
                << ", RR exposureScale=" << exposureScale
                << ", mean pre-RR luminance=";
        if (meanInputLuminanceValid)
        {
            message << meanInputLuminance;
        }
        else
        {
            message << "pending";
        }
        EngineLog::Breadcrumb("dlss-rr-exposure", message.str());
    }

    // DLSS reset on large translation only. Rotation is NOT a cut — valid motion vectors carry
    // camera rotation; the old trace-based rotation branch (~1.8°/frame) pulsed RR history during
    // ordinary look-around (gi-shimmer.md F7).
    bool DetectDlssCameraCut(const glm::mat4& currView, const MotionVectorFrameState& mvState)
    {
        if (!mvState.historyValid)
        {
            return false;
        }

        const glm::mat4 viewToViewPrev = mvState.prevView * glm::inverse(currView);
        const glm::vec3 translation(viewToViewPrev[3]);
        constexpr float kCutThresholdWorldUnits = 2.0f;
        return glm::dot(translation, translation)
            > kCutThresholdWorldUnits * kCutThresholdWorldUnits;
    }
}

DlssTemporalGuideInputs DlssResolvePass::ResolveTemporalGuideInputs(
    const DlssResolvePassInputs& inputs)
{
    DlssTemporalGuideInputs result{};
    if (inputs.sceneFramebuffer == nullptr)
    {
        return result;
    }

    constexpr std::uint32_t kPixelSrv =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const bool pathTracerDlssActive =
        inputs.pathTracerActive
        && inputs.pathTracerConvergenceMode == PtConvergenceMode::RealTime
        && inputs.pathTracerOutputResource != nullptr
        && inputs.dxrPathTracerOutputSrv != 0;

    std::uint32_t ptBundleReady = 0;
    if (pathTracerDlssActive && inputs.preparePathTracerRrBundle)
    {
        ptBundleReady = inputs.preparePathTracerRrBundle();
    }

    const int bundleMode = inputs.ptRrBundleMode;
    const bool usePtDepth = (ptBundleReady & 2u) != 0
        && inputs.ptDlssDepthTarget != nullptr
        && inputs.ptDlssDepthTarget->resource != nullptr;
    const bool usePtMotion = pathTracerDlssActive
        && inputs.pathTracerMotionResource != nullptr
        && (bundleMode == 5
            || bundleMode == 3
            || (bundleMode == 0 && usePtDepth && (ptBundleReady & 1u) != 0u));

    result.usesPathTracerDepth = usePtDepth;
    result.usesPathTracerMotion = usePtMotion;
    result.depth = usePtDepth
        ? inputs.ptDlssDepthTarget->resource
        : inputs.sceneFramebuffer->GetDepthResource();
    result.depthState = usePtDepth ? inputs.ptDlssDepthTarget->resourceState : kPixelSrv;
    result.depthSrv = usePtDepth
        ? inputs.pathTracerDepthSrv
        : inputs.sceneFramebuffer->GetDepthSrvCpuHandle();
    result.motion = inputs.sceneFramebuffer->GetGBufferColorResource(GBufferSlot::MotionVelocity);
    result.motionState = kPixelSrv;
    result.motionSrv = inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity);

    if (usePtMotion)
    {
        result.motion = inputs.pathTracerMotionResource;
        result.motionState = inputs.pathTracerMotionResourceState;
        result.motionSrv = inputs.pathTracerMotionSrv;
    }
    else if (pathTracerDlssActive && inputs.patchPathTracerSkyMotion
        && inputs.patchPathTracerSkyMotion()
        && inputs.ptDlssMotionTarget != nullptr)
    {
        result.motion = inputs.ptDlssMotionTarget->resource;
        result.motionState = inputs.ptDlssMotionTarget->resourceState;
        result.motionSrv = inputs.ptDlssMotionTarget->srvCpuHandle;
    }

    // For a static-scene diagnostic, feed zero object motion and let Streamline reconstruct the
    // camera component from this exact depth resource and clip-to-previous transform. This path is
    // deliberately exclusive with foreground dilation.
    if (inputs.reconstructDlssCameraMotion && inputs.generateZeroDlssMotion
        && inputs.dlssDilatedMotionTarget != nullptr
        && inputs.dlssDilatedMotionTarget->resource != nullptr
        && inputs.generateZeroDlssMotion())
    {
        result.motion = inputs.dlssDilatedMotionTarget->resource;
        result.motionState = inputs.dlssDilatedMotionTarget->resourceState;
        result.motionSrv = inputs.dlssDilatedMotionTarget->srvCpuHandle;
        result.cameraMotionReconstructed = true;
    }
    else if (inputs.useDilatedDlssMotionVectors && inputs.generateDilatedDlssMotion
        && inputs.dlssDilatedMotionTarget != nullptr
        && inputs.dlssDilatedMotionTarget->resource != nullptr
        && inputs.generateDilatedDlssMotion(result.depthSrv, result.motionSrv))
    {
        result.motion = inputs.dlssDilatedMotionTarget->resource;
        result.motionState = inputs.dlssDilatedMotionTarget->resourceState;
        result.motionSrv = inputs.dlssDilatedMotionTarget->srvCpuHandle;
        result.motionVectorsDilated = true;
    }

    return result;
}

glm::mat4 DlssResolvePass::BuildClipToPrevClip(const DlssResolvePassInputs& inputs)
{
    if (inputs.camera == nullptr || !inputs.motionVectorState.historyValid)
    {
        return glm::mat4(1.0f);
    }

    const glm::mat4 currentViewProjection =
        inputs.camera->GetUnjitteredProjectionMatrix() * inputs.camera->GetViewMatrix();
    return inputs.motionVectorState.prevViewProjection * glm::inverse(currentViewProjection);
}

void DlssResolvePass::Execute(
    const PostProcessContext& context,
    const DlssResolvePassInputs& inputs,
    DlssResolvePassOutputs& outputs)
{
    outputs.dlssRan = false;
    outputs.pathTracerDlssResolvedThisFrame = false;
    outputs.pathTracerOutputResourceStateValid = false;
    outputs.pathTracerOutputResourceState = 0;
    outputs.dlssHistoryValid = inputs.dlssHistoryValid;
    outputs.dlssBloomHistoryValid = inputs.dlssBloomHistoryValid;
    outputs.dlssBloomTemporalWarmupFrames = inputs.dlssBloomTemporalWarmupFrames;
    outputs.prevFrameBloomSrv = 0;

    if (inputs.camera == nullptr || inputs.sceneFramebuffer == nullptr
        || inputs.outputTarget == nullptr || inputs.dlssOutputTarget == nullptr
        || inputs.dlssOutputTarget->resource == nullptr
        || context.renderWidth <= 0 || inputs.viewportWidth <= 0)
    {
        return;
    }

    SceneRenderTrace::Section dlssSection("dlss");

    DlssContext& dlss = DlssContext::Get();
    const bool dlssUsable = dlss.IsReady() && dlss.IsRuntimeInitialized()
        && dlss.IsDlssSupported() && inputs.sceneFramebuffer->HasVelocity()
        && inputs.sceneFramebuffer->GetDepthResource() != nullptr;

    void* hdrInputResource = nullptr;
    std::uint32_t hdrInputState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const bool pathTracerDlssActive =
        inputs.pathTracerActive
        && inputs.pathTracerConvergenceMode == PtConvergenceMode::RealTime
        && inputs.pathTracerOutputResource != nullptr
        && inputs.dxrPathTracerOutputSrv != 0;
    bool ptBloomTemporalMotion = false;
    bool ptBloomTemporalDepth = false;
    if (pathTracerDlssActive)
    {
        hdrInputResource = inputs.pathTracerOutputResource;
        hdrInputState = inputs.pathTracerOutputResourceState;
        outputs.pathTracerOutputResourceStateValid = true;
        outputs.pathTracerOutputResourceState = hdrInputState;
    }
    else if (inputs.hdrCompositeTarget != nullptr
        && inputs.hdrColorSrv == inputs.hdrCompositeTarget->srvCpuHandle
        && inputs.hdrCompositeTarget->resource != nullptr)
    {
        hdrInputResource = inputs.hdrCompositeTarget->resource;
        hdrInputState = inputs.hdrCompositeTarget->resourceState;
    }
    else
    {
        hdrInputResource = inputs.sceneFramebuffer->GetGBufferColorResource(GBufferSlot::DirectLighting);
    }

    if (dlssUsable && hdrInputResource != nullptr)
    {
        SceneRenderTrace::Scope evalScope("dlss evaluate");
        auto* commandList =
            static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

        inputs.sceneFramebuffer->EnsureShaderResourceState();
        constexpr std::uint32_t kPixelSrv =
            static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (pathTracerDlssActive && hdrInputState != kPixelSrv)
        {
            TransitionResource(
                commandList,
                static_cast<ID3D12Resource*>(hdrInputResource),
                static_cast<D3D12_RESOURCE_STATES>(hdrInputState),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            hdrInputState = kPixelSrv;
            outputs.pathTracerOutputResourceState = hdrInputState;
        }

        auto* dlssOut = static_cast<ID3D12Resource*>(inputs.dlssOutputTarget->resource);
        TransitionResource(
            commandList,
            dlssOut,
            static_cast<D3D12_RESOURCE_STATES>(inputs.dlssOutputTarget->resourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        inputs.dlssOutputTarget->resourceState =
            static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DlssFrameInputs in{};
        in.commandList = commandList;
        in.colorInput = hdrInputResource;
        in.colorInputState = hdrInputState;
        in.colorOutput = dlssOut;
        in.colorOutputState = static_cast<unsigned int>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // P4b PT RR bundle selection (devdoc/dxr/pt/full-rr-guides.md + gi-shimmer.md switchboard).
        // This is also shared with the raw PT reprojection audit, so it cannot silently diverge
        // from the depth/motion resources Streamline receives.
        const DlssTemporalGuideInputs guides = ResolveTemporalGuideInputs(inputs);
        ptBloomTemporalMotion = guides.usesPathTracerMotion;
        ptBloomTemporalDepth = guides.usesPathTracerDepth;
        in.depth = guides.depth;
        in.depthState = guides.depthState;
        in.motionVectors = guides.motion;
        in.motionVectorsState = guides.motionState;
        in.motionVectorsDilated = guides.motionVectorsDilated;
        in.cameraMotionIncluded = !guides.cameraMotionReconstructed;

        if (pathTracerDlssActive && inputs.ptRrBundleMode == 0 && !guides.usesPathTracerDepth)
        {
            static bool loggedFallbackOnce = false;
            if (!loggedFallbackOnce)
            {
                loggedFallbackOnce = true;
                EngineLog::Warn(
                    "dlss",
                    "path tracer full RR bundle unavailable - using raster guide fallback");
            }
        }
        in.renderWidth = static_cast<unsigned int>(context.renderWidth);
        in.renderHeight = static_cast<unsigned int>(context.renderHeight);
        in.displayWidth = static_cast<unsigned int>(inputs.dlssOutputTarget->width);
        in.displayHeight = static_cast<unsigned int>(inputs.dlssOutputTarget->height);
        in.quality = inputs.quality;
        in.colorIsHdr = true;
        in.depthInverted = false;

        const glm::mat4 view = inputs.camera->GetViewMatrix();
        const bool cameraCut = DetectDlssCameraCut(view, inputs.motionVectorState);
        in.reset = inputs.forceDlssResetEveryFrame || !inputs.dlssHistoryValid
            || !inputs.motionVectorState.historyValid || cameraCut;
        if (cameraCut)
        {
            outputs.dlssHistoryValid = false;
        }

        const glm::vec2 mvecScale = DlssMvecScale();
        in.mvecScaleX = mvecScale.x;
        in.mvecScaleY = mvecScale.y;

        const glm::vec2 jitterPixels = JitterNdcToPixels(
            inputs.camera->GetProjectionJitter(), context.renderWidth, context.renderHeight);
        in.jitterX = jitterPixels.x;
        in.jitterY = jitterPixels.y;

        in.exposureScale = DlssExposureScaleFromEv(inputs.exposure);
        if (inputs.rayReconstructionActive)
        {
            LogRrExposureDiagnosticsOnChange(
                inputs.exposure,
                in.exposureScale,
                inputs.meanInputLuminance,
                inputs.meanInputLuminanceValid);
        }
        in.preExposure = 1.0f;
        in.sharpness = inputs.dlssSharpness;
        in.rrPreset = inputs.rrPreset;

        const glm::mat4 unjitteredProj = inputs.camera->GetUnjitteredProjectionMatrix();
        const glm::mat4 clipToPrevClip = BuildClipToPrevClip(inputs);
        std::memcpy(in.cameraViewToClip, glm::value_ptr(unjitteredProj), sizeof(float) * 16);
        const glm::mat4 clipToView = glm::inverse(unjitteredProj);
        std::memcpy(in.clipToCameraView, glm::value_ptr(clipToView), sizeof(float) * 16);
        std::memcpy(in.clipToPrevClip, glm::value_ptr(clipToPrevClip), sizeof(float) * 16);
        const glm::mat4 prevClipToClip = glm::inverse(clipToPrevClip);
        std::memcpy(in.prevClipToClip, glm::value_ptr(prevClipToClip), sizeof(float) * 16);

        in.cameraNear = inputs.camera->GetNearPlane();
        in.cameraFar = inputs.camera->GetFarPlane();
        in.cameraFovVertical = glm::radians(inputs.camera->GetFov());
        in.cameraAspect = inputs.camera->GetAspect();
        const glm::vec3 camPos = inputs.camera->GetPosition();
        const glm::vec3 camFwd = inputs.camera->GetFront();
        const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 camUp(view[0][1], view[1][1], view[2][1]);
        in.cameraPos[0] = camPos.x;
        in.cameraPos[1] = camPos.y;
        in.cameraPos[2] = camPos.z;
        in.cameraForward[0] = camFwd.x;
        in.cameraForward[1] = camFwd.y;
        in.cameraForward[2] = camFwd.z;
        in.cameraRight[0] = camRight.x;
        in.cameraRight[1] = camRight.y;
        in.cameraRight[2] = camRight.z;
        in.cameraUp[0] = camUp.x;
        in.cameraUp[1] = camUp.y;
        in.cameraUp[2] = camUp.z;

        const bool pathTracerRealTime =
            pathTracerDlssActive
        && inputs.pathTracerConvergenceMode == PtConvergenceMode::RealTime;
        const bool useRr = dlss.IsRrSupported()
            && inputs.sceneFramebuffer->HasMaterialGbuffer()
            && inputs.rrNormalRoughnessTarget != nullptr
            && inputs.rrNormalRoughnessTarget->resource != nullptr
            && inputs.rayReconstructionActive;
        if (useRr && inputs.generateRrGuides)
        {
            {
                const GfxContext::GpuTimerScope gpuScopeRrGuides("DLSS/RR guides");
                inputs.generateRrGuides();
            }
            in.useRayReconstruction = true;
            in.diffuseAlbedo = inputs.rrDiffuseAlbedoTarget->resource;
            in.diffuseAlbedoState = inputs.rrDiffuseAlbedoTarget->resourceState;
            in.specularAlbedo = inputs.rrSpecularAlbedoTarget->resource;
            in.specularAlbedoState = inputs.rrSpecularAlbedoTarget->resourceState;
            in.normalRoughness = inputs.rrNormalRoughnessTarget->resource;
            in.normalRoughnessState = inputs.rrNormalRoughnessTarget->resourceState;
            const bool ptSpecGuideActive =
                pathTracerRealTime && inputs.dxrPathTracerOutputSrv != 0;
            if ((inputs.dxrReflectionSrv != 0 || ptSpecGuideActive)
                && inputs.rrSpecularHitDistanceTarget != nullptr
                && inputs.rrSpecularHitDistanceTarget->resource != nullptr)
            {
                in.specularHitDistance = inputs.rrSpecularHitDistanceTarget->resource;
                in.specularHitDistanceState = inputs.rrSpecularHitDistanceTarget->resourceState;
            }
            std::memcpy(in.worldToCameraView, glm::value_ptr(view), sizeof(float) * 16);
            const glm::mat4 viewToWorld = glm::inverse(view);
            std::memcpy(in.cameraViewToWorld, glm::value_ptr(viewToWorld), sizeof(float) * 16);
        }

        {
            const GfxContext::GpuTimerScope gpuScopeEvaluate("DLSS/Evaluate");
            outputs.dlssRan = dlss.Evaluate(in);
            GfxContext::Get().RebindFrameDescriptorHeaps();

            if (outputs.dlssRan && pathTracerDlssActive)
            {
                outputs.pathTracerDlssResolvedThisFrame = true;
            }

            if (outputs.dlssRan)
            {
                TransitionResource(
                    commandList,
                    dlssOut,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                inputs.dlssOutputTarget->resourceState = kPixelSrv;
                outputs.dlssHistoryValid = true;
            }
            else
            {
                TransitionResource(
                    commandList,
                    dlssOut,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                inputs.dlssOutputTarget->resourceState = kPixelSrv;
                outputs.dlssHistoryValid = false;
            }
        }
        evalScope.Success();
    }

    if (outputs.dlssRan)
    {
        // DLSS evaluate applies exposureScale; bloom/tonemap must not re-apply EV (F6).
        constexpr float kPostDlssExposureEv = 0.0f;

        if (pathTracerDlssActive && inputs.pathTracerGridOverlayEnabled
            && inputs.drawPathTracerGridOverlay)
        {
            const GfxContext::GpuTimerScope gpuScopePtOverlay("DLSS/PT overlay");
            inputs.drawPathTracerGridOverlay(
                *inputs.dlssOutputTarget,
                inputs.viewportWidth,
                inputs.viewportHeight);
        }

        std::uintptr_t displayBloomSrv = 0;
        if (inputs.bloomEnabled && inputs.dlssBloomExtractTarget != nullptr
            && inputs.dlssBloomExtractTarget->srvCpuHandle != 0)
        {
            // DLSS evaluate applies exposureScale; bloom/tonemap must not re-apply EV (F6).
            DisplayResBloomInputs bloomInputs{};
            bloomInputs.hdrColorSrv = inputs.dlssOutputTarget->srvCpuHandle;
            bloomInputs.displayWidth = inputs.viewportWidth;
            bloomInputs.displayHeight = inputs.viewportHeight;
            bloomInputs.renderWidth = context.renderWidth;
            bloomInputs.renderHeight = context.renderHeight;
            bloomInputs.exposure = kPostDlssExposureEv;
            bloomInputs.bloomThreshold = inputs.bloomThreshold;
            bloomInputs.bloomSoftKnee = inputs.bloomSoftKnee;
            bloomInputs.bloomBlurRadius = inputs.bloomBlurRadius;
            bloomInputs.bloomTemporalBlendFactor = inputs.bloomTemporalBlendFactor;
            bloomInputs.bloomSameUvBlendFactor = inputs.bloomSameUvBlendFactor;
            bloomInputs.bloomDepthThreshold = inputs.bloomDepthThreshold;
            // PT HDR disagrees with raster gbuffer — attenuation caused dark rims on mirrors.
            bloomInputs.useMaterialGbuffer =
                inputs.sceneFramebuffer->HasMaterialGbuffer() && !pathTracerDlssActive;
            if (bloomInputs.useMaterialGbuffer)
            {
                bloomInputs.material0Srv = inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(
                    GBufferSlot::MaterialAlbedoRough);
                bloomInputs.material1Srv = inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(
                    GBufferSlot::MaterialMetallic);
            }
            const bool usePtBloomTemporalGuides = pathTracerDlssActive && ptBloomTemporalMotion
                && ptBloomTemporalDepth && inputs.pathTracerMotionSrv != 0
                && inputs.pathTracerDepthSrv != 0;
            bloomInputs.hasVelocity = usePtBloomTemporalGuides
                || inputs.sceneFramebuffer->HasVelocity();
            bloomInputs.velocitySrv = usePtBloomTemporalGuides
                ? inputs.pathTracerMotionSrv
                : inputs.sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity);
            bloomInputs.depthSrv = usePtBloomTemporalGuides
                ? inputs.pathTracerDepthSrv
                : inputs.sceneFramebuffer->GetDepthSrvCpuHandle();
            bloomInputs.bloomExtractShader = inputs.bloomExtractShader;
            bloomInputs.bloomBlurShader = inputs.bloomBlurShader;
            bloomInputs.bloomTemporalShader = inputs.bloomTemporalShader;
            bloomInputs.bloomExtractTarget = inputs.dlssBloomExtractTarget;
            bloomInputs.bloomBlurTarget = inputs.dlssBloomBlurTarget;
            bloomInputs.bloomBlur2Target = inputs.dlssBloomBlur2Target;
            bloomInputs.bloomTemporalTarget = inputs.dlssBloomTemporalTarget;
            bloomInputs.bloomHistoryTarget = inputs.dlssBloomHistoryTarget;
            bloomInputs.bloomHistoryValid = inputs.dlssBloomHistoryValid;
            bloomInputs.bloomTemporalWarmupFrames = inputs.dlssBloomTemporalWarmupFrames;

            DisplayResBloomOutputs bloomOutputs{};
            {
                const GfxContext::GpuTimerScope gpuScopeDisplayBloom("DLSS/Display bloom");
                if (BloomTonemapPass::ExecuteDisplayResBloom(context, bloomInputs, bloomOutputs))
                {
                    displayBloomSrv = bloomOutputs.bloomSrv;
                    outputs.dlssBloomHistoryValid = bloomOutputs.bloomHistoryValid;
                    outputs.dlssBloomTemporalWarmupFrames = bloomOutputs.bloomTemporalWarmupFrames;
                }
            }
        }

        {
            const GfxContext::GpuTimerScope gpuScopeTonemap("DLSS/Tonemap");
            BloomTonemapPass::ExecuteTonemapDlssDisplay(
                context,
                inputs.outputTarget,
                inputs.viewportWidth,
                inputs.viewportHeight,
                inputs.dlssOutputTarget->srvCpuHandle,
                displayBloomSrv,
                kPostDlssExposureEv,
                inputs.tonemapMode,
                inputs.bloomIntensity,
                inputs.tonemapShader);
        }

        outputs.prevFrameBloomSrv = displayBloomSrv;
    }
    else
    {
        SceneRenderTrace::Scope fallbackScope("dlss fallback tonemap");
        {
            const GfxContext::GpuTimerScope gpuScopeFallback("DLSS/Fallback tonemap");
            BloomTonemapPass::ExecuteTonemapToViewport(
                context,
                inputs.outputTarget,
                inputs.viewportWidth,
                inputs.viewportHeight,
                inputs.fallbackTonemapInputs);
        }
        fallbackScope.Success();
        outputs.prevFrameBloomSrv = 0;
    }

    dlssSection.Success();
}
