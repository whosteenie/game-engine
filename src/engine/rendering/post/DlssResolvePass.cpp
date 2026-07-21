#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rendering/post/reconstruction/ExposurePolicy.h"

#include "engine/platform/diagnostics/FrameDiagnostics.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"
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

    const char* DlssTraceQuality(const DlssQuality quality)
    {
        switch (quality)
        {
        case DlssQuality::DLAA: return "dlaa";
        case DlssQuality::Quality: return "quality";
        case DlssQuality::Balanced: return "balanced";
        case DlssQuality::Performance: return "performance";
        case DlssQuality::UltraPerformance: return "ultra-performance";
        }
        return "unknown";
    }

    void LogExposureDiagnosticsOnChange(
        const bool rayReconstruction,
        const float authoredDisplayEv,
        const ReconstructionExposurePolicy& policy,
        const float meanInputLuminance,
        const bool meanInputLuminanceValid)
    {
        static bool lastRayReconstruction = false;
        static float lastDisplayEv = std::numeric_limits<float>::quiet_NaN();
        static bool lastMeanValid = false;
        if (rayReconstruction == lastRayReconstruction
            && authoredDisplayEv == lastDisplayEv
            && meanInputLuminanceValid == lastMeanValid)
        {
            return;
        }

        lastRayReconstruction = rayReconstruction;
        lastDisplayEv = authoredDisplayEv;
        lastMeanValid = meanInputLuminanceValid;
        std::ostringstream message;
        message << std::fixed << std::setprecision(5) << "feature="
                << (rayReconstruction ? "rr" : "dlss")
                << ", authored display EV=" << authoredDisplayEv
                << ", reconstruction preExposure=";
        if (rayReconstruction)
        {
            message << "omitted, reconstruction exposureScale=omitted";
        }
        else
        {
            message << policy.reconstructionPreExposure
                    << ", reconstruction exposureScale="
                    << policy.reconstructionExposureScale;
        }
        message << ", bloom EV=" << policy.bloomExposureEv
                << ", tonemap EV=" << policy.displayExposureEv
                << ", mean pre-reconstruction luminance=";
        if (meanInputLuminanceValid)
        {
            message << meanInputLuminance;
        }
        else
        {
            message << "pending";
        }
        EngineLog::Breadcrumb("reconstruction-exposure", message.str());
    }

    void CopyDlssCommonEvaluationFields(
        const DlssFrameInputs& source,
        DlssFrameInputs& destination)
    {
        destination.commandList = source.commandList;
        destination.extentPlan = source.extentPlan;
        destination.renderWidth = source.renderWidth;
        destination.renderHeight = source.renderHeight;
        destination.displayWidth = source.displayWidth;
        destination.displayHeight = source.displayHeight;
        destination.viewportId = source.viewportId;
        destination.quality = source.quality;
        destination.colorIsHdr = source.colorIsHdr;
        destination.depthInverted = source.depthInverted;
        destination.mvecScaleX = source.mvecScaleX;
        destination.mvecScaleY = source.mvecScaleY;
        destination.jitterX = source.jitterX;
        destination.jitterY = source.jitterY;
        destination.cameraNear = source.cameraNear;
        destination.cameraFar = source.cameraFar;
        destination.cameraFovVertical = source.cameraFovVertical;
        destination.cameraAspect = source.cameraAspect;
        destination.exposureScale = source.exposureScale;
        destination.preExposure = source.preExposure;
        destination.sharpness = source.sharpness;
        destination.useRayReconstruction = source.useRayReconstruction;
        destination.rrPreset = source.rrPreset;
        std::memcpy(destination.cameraViewToClip, source.cameraViewToClip, sizeof(source.cameraViewToClip));
        std::memcpy(destination.clipToCameraView, source.clipToCameraView, sizeof(source.clipToCameraView));
        std::memcpy(destination.clipToPrevClip, source.clipToPrevClip, sizeof(source.clipToPrevClip));
        std::memcpy(destination.prevClipToClip, source.prevClipToClip, sizeof(source.prevClipToClip));
        std::memcpy(destination.cameraPos, source.cameraPos, sizeof(source.cameraPos));
        std::memcpy(destination.cameraRight, source.cameraRight, sizeof(source.cameraRight));
        std::memcpy(destination.cameraUp, source.cameraUp, sizeof(source.cameraUp));
        std::memcpy(destination.cameraForward, source.cameraForward, sizeof(source.cameraForward));
        std::memcpy(destination.worldToCameraView, source.worldToCameraView, sizeof(source.worldToCameraView));
        std::memcpy(destination.cameraViewToWorld, source.cameraViewToWorld, sizeof(source.cameraViewToWorld));
    }

    // DLSS reset on large translation only. Rotation is NOT a cut — valid motion vectors carry
    // camera rotation; the old trace-based rotation branch (~1.8°/frame) pulsed RR history during
    // ordinary look-around (gi-shimmer.md F7).
    bool DetectCameraCutImpl(const glm::mat4& currView, const MotionVectorFrameState& mvState)
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

bool DlssResolvePass::DetectCameraCut(
    const glm::mat4& currentView,
    const MotionVectorFrameState& motionState)
{
    return DetectCameraCutImpl(currentView, motionState);
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
    result.pathTracerBundleReady = ptBundleReady;

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
        ? inputs.ptDlssDepthTarget->srvCpuHandle
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
    bool motionAlreadySupported = false;
    if (!usePtMotion && pathTracerDlssActive && inputs.patchPathTracerSkyMotion
        && inputs.patchPathTracerSkyMotion()
        && inputs.ptDlssMotionTarget != nullptr)
    {
        result.motion = inputs.ptDlssMotionTarget->resource;
        result.motionState = inputs.ptDlssMotionTarget->resourceState;
        result.motionSrv = inputs.ptDlssMotionTarget->srvCpuHandle;
        motionAlreadySupported = true;
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
        motionAlreadySupported = true;
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
        motionAlreadySupported = true;
    }
    else if (!motionAlreadySupported && inputs.generateSupportedDlssMotion
        && inputs.dlssDilatedMotionTarget != nullptr
        && inputs.dlssDilatedMotionTarget->resource != nullptr
        && inputs.generateSupportedDlssMotion(result.motionSrv))
    {
        result.motion = inputs.dlssDilatedMotionTarget->resource;
        result.motionState = inputs.dlssDilatedMotionTarget->resourceState;
        result.motionSrv = inputs.dlssDilatedMotionTarget->srvCpuHandle;
        motionAlreadySupported = true;
    }

    if (!motionAlreadySupported)
    {
        result.motion = nullptr;
        result.motionState = 0;
        result.motionSrv = 0;
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
    outputs.opticalTransmissionHistoryValid = inputs.opticalTransmissionHistoryValid;
    outputs.dlssBloomHistoryValid = inputs.dlssBloomHistoryValid;
    outputs.dlssBloomTemporalWarmupFrames = inputs.dlssBloomTemporalWarmupFrames;
    outputs.prevFrameBloomSrv = 0;
    PostProcessTarget* resolvedDlssTarget = inputs.dlssOutputTarget;

    if (inputs.camera == nullptr || inputs.sceneFramebuffer == nullptr
        || inputs.outputTarget == nullptr || inputs.dlssOutputTarget == nullptr
        || inputs.dlssOutputTarget->resource == nullptr
        || context.renderWidth <= 0 || inputs.viewportWidth <= 0)
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.dlssViewportId,
            inputs.rayReconstructionActive ? "rr" : "dlss",
            DlssTraceQuality(inputs.quality),
            "skipped",
            "invalid-resolve-input",
            false,
            0,
            false,
            0);
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
        const bool opticalLayerSplit = pathTracerDlssActive
            && inputs.independentOpticalRrLayers
            && inputs.rayReconstructionActive
            && dlss.IsRrSupported()
            && inputs.generateRrGuides
            && (guides.pathTracerBundleReady & 4u) != 0u
            && inputs.pathTracerOpticalTransmissionOutputResource != nullptr
            && inputs.pathTracerOpticalTransmissionOutputSrv != 0
            && inputs.pathTracerOpticalTransmissionMotionSrv != 0
            && inputs.ptOpticalReflectionInputTarget != nullptr
            && inputs.ptOpticalReflectionInputTarget->resource != nullptr
            && inputs.dlssOpticalTransmissionOutputTarget != nullptr
            && inputs.dlssOpticalTransmissionOutputTarget->resource != nullptr
            && inputs.dlssOpticalCompositeTarget != nullptr
            && inputs.ptOpticalTransmissionDlssDepthTarget != nullptr
            && inputs.ptOpticalTransmissionDlssDepthTarget->resource != nullptr
            && inputs.ptDlssMotionTarget != nullptr
            && inputs.ptDlssMotionTarget->resource != nullptr
            && inputs.dlssOpticalTransmissionMotionTarget != nullptr
            && inputs.dlssOpticalTransmissionMotionTarget->resource != nullptr
            && inputs.rrOpticalTransmissionDiffuseAlbedoTarget != nullptr
            && inputs.rrOpticalTransmissionSpecularAlbedoTarget != nullptr
            && inputs.rrOpticalTransmissionNormalRoughnessTarget != nullptr
            && inputs.ptOpticalLayersShader != nullptr;
        const bool psrSurfaceReplacement = pathTracerDlssActive
            && inputs.mirrorChainPsr
            && inputs.rayReconstructionActive
            && inputs.pathTracerPsrThroughputSrv != 0
            && inputs.ptOpticalReflectionInputTarget != nullptr
            && inputs.ptOpticalReflectionInputTarget->resource != nullptr
            && inputs.dlssOpticalCompositeTarget != nullptr
            && inputs.dlssOpticalCompositeTarget->resource != nullptr
            && inputs.ptOpticalLayersShader != nullptr;
        if (pathTracerDlssActive && inputs.rayReconstructionActive && !opticalLayerSplit)
        {
            outputs.opticalTransmissionHistoryValid = false;
        }
        if (opticalLayerSplit || psrSurfaceReplacement)
        {
            const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
            inputs.ptOpticalLayersShader->Use(false, false);
            inputs.ptOpticalLayersShader->SetInt(
                "uComposite",
                psrSurfaceReplacement ? (opticalLayerSplit ? 6 : 4) : 0);
            inputs.ptOpticalLayersShader->BindTextureSlot(0, inputs.dxrPathTracerOutputSrv);
            inputs.ptOpticalLayersShader->BindTextureSlot(
                1, inputs.pathTracerOpticalTransmissionOutputSrv);
            if (psrSurfaceReplacement)
            {
                inputs.ptOpticalLayersShader->BindTextureSlot(
                    2, inputs.pathTracerPsrThroughputSrv);
            }
            context.draw.DrawFullscreenToTarget(
                *inputs.ptOpticalLayersShader,
                *inputs.ptOpticalReflectionInputTarget,
                context.renderWidth,
                context.renderHeight,
                clear);
            in.colorInput = inputs.ptOpticalReflectionInputTarget->resource;
            in.colorInputState = inputs.ptOpticalReflectionInputTarget->resourceState;
        }
        in.depth = guides.depth;
        in.depthState = guides.depthState;
        in.motionVectors = guides.motion;
        in.motionVectorsState = guides.motionState;
        in.motionVectorsDilated = guides.motionVectorsDilated;
        in.cameraMotionIncluded = !guides.cameraMotionReconstructed;
        in.extentPlan = inputs.plannedExtent;

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
        in.viewportId = inputs.dlssViewportId;
        in.quality = inputs.quality;
        in.colorIsHdr = true;
        in.depthInverted = false;

        const glm::mat4 view = inputs.camera->GetViewMatrix();
        const bool cameraCut = inputs.cameraCutKnown
            ? inputs.cameraCut
            : DetectCameraCut(view, inputs.motionVectorState);
        in.reset = inputs.forceDlssResetEveryFrame || !inputs.dlssHistoryValid
            || !inputs.motionVectorState.historyValid || cameraCut;
        const std::uint32_t resetReasonBits =
            (inputs.forceDlssResetEveryFrame ? 1u : 0u)
            | (!inputs.dlssHistoryValid ? 2u : 0u)
            | (!inputs.motionVectorState.historyValid ? 4u : 0u)
            | (cameraCut ? 8u : 0u);
        FrameDiagnostics::LogHistoryEvent(
            inputs.dlssViewportId,
            "reconstruction",
            in.reset ? "request" : "consume",
            pathTracerDlssActive ? "path-tracer" : "raster",
            guides.usesPathTracerDepth || guides.usesPathTracerMotion ? "pt-guide-bundle" : "raster-guides",
            inputs.rayReconstructionActive ? "rr" : "dlss",
            DlssTraceQuality(inputs.quality),
            context.renderWidth,
            context.renderHeight,
            inputs.viewportWidth,
            inputs.viewportHeight,
            cameraCut,
            inputs.forceDlssResetEveryFrame || inputs.useDilatedDlssMotionVectors
                || inputs.reconstructDlssCameraMotion,
            resetReasonBits);
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

        const ReconstructionExposurePolicy exposurePolicy =
            ResolveReconstructionExposurePolicy(inputs.exposure);
        in.preExposure = exposurePolicy.reconstructionPreExposure;
        in.exposureScale = exposurePolicy.reconstructionExposureScale;
        LogExposureDiagnosticsOnChange(
            inputs.rayReconstructionActive,
            inputs.exposure,
            exposurePolicy,
            inputs.meanInputLuminance,
            inputs.meanInputLuminanceValid);
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
            if (!inputs.mirrorChainPsr
                && (inputs.dxrReflectionSrv != 0 || ptSpecGuideActive)
                && inputs.rrSpecularHitDistanceTarget != nullptr
                && inputs.rrSpecularHitDistanceTarget->resource != nullptr)
            {
                in.specularHitDistance = inputs.rrSpecularHitDistanceTarget->resource;
                in.specularHitDistanceState = inputs.rrSpecularHitDistanceTarget->resourceState;
            }
            // PSR makes the virtual receiver the primary bundle.  Duplicating that same vector as
            // optional specular motion does not describe a distinct reflected geometry domain.
            in.specularMotionVectors = nullptr;
            in.specularMotionVectorsState = 0;
            if (pathTracerRealTime && inputs.prepareRrTemporalValidity
                && inputs.pathTracerRrPrimaryOwnerSrv != 0 && guides.depthSrv != 0
                && guides.motionSrv != 0)
            {
                RrTemporalValidityInputs validity{};
                validity.historyValid = !in.reset;
                validity.depthSrv = guides.depthSrv;
                validity.normalRoughnessSrv = inputs.rrNormalRoughnessTarget->srvCpuHandle;
                validity.ownerSrv = inputs.pathTracerRrPrimaryOwnerSrv;
                validity.ownerResource = inputs.pathTracerRrPrimaryOwnerResource;
                validity.ownerResourceState = inputs.pathTracerRrPrimaryOwnerResourceState;
                validity.motionSrv = guides.motionSrv;
                validity.mvecScaleX = in.mvecScaleX;
                validity.mvecScaleY = in.mvecScaleY;
                const glm::vec2 currentJitterNdc = inputs.camera->GetProjectionJitter();
                const glm::vec2 previousJitterNdc =
                    inputs.motionVectorState.historyValid
                    && TemporalCamera::IsComplete(inputs.motionVectorState.previousCamera)
                    ? inputs.motionVectorState.previousCamera.jitterNdc
                    : currentJitterNdc;
                validity.currentJitterNdcX = currentJitterNdc.x;
                validity.currentJitterNdcY = currentJitterNdc.y;
                validity.previousJitterNdcX = previousJitterNdc.x;
                validity.previousJitterNdcY = previousJitterNdc.y;
                std::memcpy(
                    validity.clipToPrevClip,
                    in.clipToPrevClip,
                    sizeof(validity.clipToPrevClip));
                const RrTemporalValidityResult mask = inputs.prepareRrTemporalValidity(validity);
                if (inputs.rrTemporalValidityEnabled && mask.ready && mask.maskSrv != 0
                    && inputs.generateValidityFilteredRrMotion
                    && inputs.rrTemporalPrimaryMotionTarget != nullptr
                    && inputs.rrTemporalPrimaryMotionTarget->resource != nullptr
                    && inputs.generateValidityFilteredRrMotion(
                        false, validity.motionSrv, mask.maskSrv))
                {
                    in.motionVectors = inputs.rrTemporalPrimaryMotionTarget->resource;
                    in.motionVectorsState =
                        inputs.rrTemporalPrimaryMotionTarget->resourceState;
                    in.motionVectorsDilated = false;
                }
            }
            std::memcpy(in.worldToCameraView, glm::value_ptr(view), sizeof(float) * 16);
            const glm::mat4 viewToWorld = glm::inverse(view);
            std::memcpy(in.cameraViewToWorld, glm::value_ptr(viewToWorld), sizeof(float) * 16);
        }

        {
            const GfxContext::GpuTimerScope gpuScopeEvaluate("DLSS/Evaluate");
            const bool primaryRan = dlss.Evaluate(dlss.CurrentFrameToken(), in);
            GfxContext::Get().RebindFrameDescriptorHeaps();
            TransitionResource(
                commandList,
                dlssOut,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            inputs.dlssOutputTarget->resourceState = kPixelSrv;
            outputs.dlssHistoryValid = primaryRan;
            outputs.dlssRan = primaryRan;

            const bool transmissionMotionReady = primaryRan && opticalLayerSplit
                && in.useRayReconstruction
                && inputs.generateSupportedOpticalTransmissionDlssMotion
                && inputs.generateSupportedOpticalTransmissionDlssMotion(
                    inputs.pathTracerOpticalTransmissionMotionSrv);
            if (transmissionMotionReady)
            {
                DlssFrameInputs txIn{};
                CopyDlssCommonEvaluationFields(in, txIn);
                constexpr std::uint32_t kOpticalTransmissionViewportBit = 0x80000000u;
                txIn.viewportId = inputs.dlssViewportId ^ kOpticalTransmissionViewportBit;
                txIn.extentPlan.key.viewportId = txIn.viewportId;
                txIn.colorInput = inputs.pathTracerOpticalTransmissionOutputResource;
                txIn.colorInputState = inputs.pathTracerOpticalTransmissionOutputResourceState;
                auto* txOut = static_cast<ID3D12Resource*>(
                    inputs.dlssOpticalTransmissionOutputTarget->resource);
                TransitionResource(
                    commandList,
                    txOut,
                    static_cast<D3D12_RESOURCE_STATES>(
                        inputs.dlssOpticalTransmissionOutputTarget->resourceState),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                inputs.dlssOpticalTransmissionOutputTarget->resourceState =
                    static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                txIn.colorOutput = txOut;
                txIn.colorOutputState =
                    static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                txIn.depth = inputs.ptOpticalTransmissionDlssDepthTarget->resource;
                txIn.depthState = inputs.ptOpticalTransmissionDlssDepthTarget->resourceState;
                txIn.motionVectors = inputs.dlssOpticalTransmissionMotionTarget->resource;
                txIn.motionVectorsState = inputs.dlssOpticalTransmissionMotionTarget->resourceState;
                txIn.diffuseAlbedo = inputs.rrOpticalTransmissionDiffuseAlbedoTarget->resource;
                txIn.diffuseAlbedoState = inputs.rrOpticalTransmissionDiffuseAlbedoTarget->resourceState;
                txIn.specularAlbedo = inputs.rrOpticalTransmissionSpecularAlbedoTarget->resource;
                txIn.specularAlbedoState = inputs.rrOpticalTransmissionSpecularAlbedoTarget->resourceState;
                txIn.normalRoughness = inputs.rrOpticalTransmissionNormalRoughnessTarget->resource;
                txIn.normalRoughnessState = inputs.rrOpticalTransmissionNormalRoughnessTarget->resourceState;
                // All optional resources are absent by construction until this evaluation owns
                // them explicitly. In particular, primary PSR motion/hit-distance cannot leak here.
                txIn.specularHitDistance = nullptr;
                txIn.specularMotionVectors = nullptr;
                txIn.responsivityMask = nullptr;
                txIn.disocclusionMask = nullptr;
                txIn.reset = inputs.forceDlssResetEveryFrame
                    || !inputs.opticalTransmissionHistoryValid
                    || !inputs.motionVectorState.historyValid || cameraCut;
                FrameDiagnostics::LogHistoryEvent(
                    txIn.viewportId,
                    "reconstruction-optical-transmission",
                    txIn.reset ? "request" : "consume",
                    "path-tracer",
                    "pt-transmission-guide-bundle",
                    "rr",
                    DlssTraceQuality(inputs.quality),
                    context.renderWidth,
                    context.renderHeight,
                    inputs.viewportWidth,
                    inputs.viewportHeight,
                    cameraCut,
                    false,
                    txIn.reset ? 1u : 0u);
                if (inputs.prepareRrTemporalValidity
                    && inputs.pathTracerRrTransmissionOwnerSrv != 0
                    && inputs.ptOpticalTransmissionDlssDepthTarget->srvCpuHandle != 0
                    && inputs.dlssOpticalTransmissionMotionTarget->srvCpuHandle != 0)
                {
                    RrTemporalValidityInputs validity{};
                    validity.transmission = true;
                    validity.historyValid = !txIn.reset;
                    validity.depthSrv = inputs.ptOpticalTransmissionDlssDepthTarget->srvCpuHandle;
                    validity.normalRoughnessSrv =
                        inputs.rrOpticalTransmissionNormalRoughnessTarget->srvCpuHandle;
                    validity.ownerSrv = inputs.pathTracerRrTransmissionOwnerSrv;
                    validity.ownerResource = inputs.pathTracerRrTransmissionOwnerResource;
                    validity.ownerResourceState =
                        inputs.pathTracerRrTransmissionOwnerResourceState;
                    validity.motionSrv = inputs.dlssOpticalTransmissionMotionTarget->srvCpuHandle;
                    validity.mvecScaleX = txIn.mvecScaleX;
                    validity.mvecScaleY = txIn.mvecScaleY;
                    const glm::vec2 currentJitterNdc = inputs.camera->GetProjectionJitter();
                    const glm::vec2 previousJitterNdc =
                        inputs.motionVectorState.historyValid
                        && TemporalCamera::IsComplete(inputs.motionVectorState.previousCamera)
                        ? inputs.motionVectorState.previousCamera.jitterNdc
                        : currentJitterNdc;
                    validity.currentJitterNdcX = currentJitterNdc.x;
                    validity.currentJitterNdcY = currentJitterNdc.y;
                    validity.previousJitterNdcX = previousJitterNdc.x;
                    validity.previousJitterNdcY = previousJitterNdc.y;
                    std::memcpy(
                        validity.clipToPrevClip,
                        txIn.clipToPrevClip,
                        sizeof(validity.clipToPrevClip));
                    const RrTemporalValidityResult mask =
                        inputs.prepareRrTemporalValidity(validity);
                    if (inputs.rrTemporalValidityEnabled && mask.ready && mask.maskSrv != 0
                        && inputs.generateValidityFilteredRrMotion
                        && inputs.rrTemporalTransmissionMotionTarget != nullptr
                        && inputs.rrTemporalTransmissionMotionTarget->resource != nullptr
                        && inputs.generateValidityFilteredRrMotion(
                            true, validity.motionSrv, mask.maskSrv))
                    {
                        txIn.motionVectors =
                            inputs.rrTemporalTransmissionMotionTarget->resource;
                        txIn.motionVectorsState =
                            inputs.rrTemporalTransmissionMotionTarget->resourceState;
                        txIn.motionVectorsDilated = false;
                    }
                }
                const bool transmissionRan = dlss.Evaluate(dlss.CurrentFrameToken(), txIn);
                GfxContext::Get().RebindFrameDescriptorHeaps();
                TransitionResource(
                    commandList,
                    txOut,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                inputs.dlssOpticalTransmissionOutputTarget->resourceState = kPixelSrv;
                outputs.opticalTransmissionHistoryValid = transmissionRan;
                outputs.dlssRan = outputs.dlssRan || transmissionRan;

                if (transmissionRan)
                {
                    const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
                    inputs.ptOpticalLayersShader->Use(false, false);
                    inputs.ptOpticalLayersShader->SetInt(
                        "uComposite", psrSurfaceReplacement ? 7 : 1);
                    inputs.ptOpticalLayersShader->BindTextureSlot(
                        0, inputs.dlssOutputTarget->srvCpuHandle);
                    inputs.ptOpticalLayersShader->BindTextureSlot(
                        1, inputs.dlssOpticalTransmissionOutputTarget->srvCpuHandle);
                    if (psrSurfaceReplacement)
                    {
                        inputs.ptOpticalLayersShader->BindTextureSlot(
                            2, inputs.pathTracerPsrThroughputSrv);
                    }
                    context.draw.DrawFullscreenToTarget(
                        *inputs.ptOpticalLayersShader,
                        *inputs.dlssOpticalCompositeTarget,
                        inputs.viewportWidth,
                        inputs.viewportHeight,
                        clear);
                    resolvedDlssTarget = inputs.dlssOpticalCompositeTarget;
                }
            }
            else if (primaryRan && opticalLayerSplit)
            {
                outputs.dlssRan = false;
                outputs.opticalTransmissionHistoryValid = false;
            }

            if (primaryRan && psrSurfaceReplacement && !opticalLayerSplit)
            {
                const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
                inputs.ptOpticalLayersShader->Use(false, false);
                inputs.ptOpticalLayersShader->SetInt("uComposite", 5);
                inputs.ptOpticalLayersShader->BindTextureSlot(
                    0, inputs.dlssOutputTarget->srvCpuHandle);
                // Mode 5 does not consume transmission, but keep every declared slot valid.
                inputs.ptOpticalLayersShader->BindTextureSlot(
                    1, inputs.dlssOutputTarget->srvCpuHandle);
                inputs.ptOpticalLayersShader->BindTextureSlot(
                    2, inputs.pathTracerPsrThroughputSrv);
                context.draw.DrawFullscreenToTarget(
                    *inputs.ptOpticalLayersShader,
                    *inputs.dlssOpticalCompositeTarget,
                    inputs.viewportWidth,
                    inputs.viewportHeight,
                    clear);
                resolvedDlssTarget = inputs.dlssOpticalCompositeTarget;
            }

            if (outputs.dlssRan && pathTracerDlssActive)
            {
                outputs.pathTracerDlssResolvedThisFrame = true;
            }
        }
        evalScope.Success();
    }
    else
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.dlssViewportId,
            inputs.rayReconstructionActive ? "rr" : "dlss",
            DlssTraceQuality(inputs.quality),
            "skipped",
            dlssUsable ? "missing-hdr-input" : "streamline-unavailable",
            false,
            0,
            false,
            0);
    }

    if (outputs.dlssRan)
    {
        const ReconstructionExposurePolicy exposurePolicy =
            ResolveReconstructionExposurePolicy(inputs.exposure);

        if (pathTracerDlssActive && inputs.pathTracerGridOverlayEnabled
            && inputs.drawPathTracerGridOverlay)
        {
            const GfxContext::GpuTimerScope gpuScopePtOverlay("DLSS/PT overlay");
            inputs.drawPathTracerGridOverlay(
                *resolvedDlssTarget,
                inputs.viewportWidth,
                inputs.viewportHeight);
        }

        std::uintptr_t displayBloomSrv = 0;
        if (inputs.bloomEnabled && inputs.dlssBloomExtractTarget != nullptr
            && inputs.dlssBloomExtractTarget->srvCpuHandle != 0)
        {
            DisplayResBloomInputs bloomInputs{};
            bloomInputs.hdrColorSrv = resolvedDlssTarget->srvCpuHandle;
            bloomInputs.displayWidth = inputs.viewportWidth;
            bloomInputs.displayHeight = inputs.viewportHeight;
            bloomInputs.renderWidth = context.renderWidth;
            bloomInputs.renderHeight = context.renderHeight;
            // Bloom extraction owns display EV for this branch. It remains in the same
            // display-exposed linear HDR space as the base branch entering tonemapping.
            bloomInputs.exposure = exposurePolicy.bloomExposureEv;
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
            const bool displayBloomCanConsumeHistory =
                context.renderWidth == inputs.viewportWidth
                && context.renderHeight == inputs.viewportHeight
                && bloomInputs.hasVelocity
                && bloomInputs.bloomTemporalShader != nullptr
                && bloomInputs.bloomTemporalTarget != nullptr
                && bloomInputs.bloomHistoryTarget != nullptr
                && bloomInputs.bloomTemporalTarget->srvCpuHandle != 0;
            FrameDiagnostics::LogHistoryEvent(
                inputs.dlssViewportId, "dlss-display-bloom",
                !displayBloomCanConsumeHistory ? "skip"
                    : (inputs.dlssBloomHistoryValid ? "consume" : "request"),
                pathTracerDlssActive ? "path-tracer" : "raster",
                ptBloomTemporalMotion || ptBloomTemporalDepth ? "pt-guide-bundle" : "raster-guides",
                inputs.rayReconstructionActive ? "rr" : "dlss", DlssTraceQuality(inputs.quality),
                context.renderWidth, context.renderHeight, inputs.viewportWidth, inputs.viewportHeight,
                false, false, !displayBloomCanConsumeHistory ? 0u
                    : (inputs.dlssBloomHistoryValid ? 0u : 1u));
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
                resolvedDlssTarget->srvCpuHandle,
                displayBloomSrv,
                exposurePolicy.displayExposureEv,
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
