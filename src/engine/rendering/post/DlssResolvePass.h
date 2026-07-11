#pragma once

#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/post/BloomTonemapPass.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"
#include "engine/rhi/DlssContext.h"

#include <cstdint>
#include <functional>

class Camera;
class Framebuffer;

struct DlssResolvePassInputs
{
    const Camera* camera = nullptr;
    Framebuffer* sceneFramebuffer = nullptr;
    const Framebuffer* outputTarget = nullptr;
    MotionVectorFrameState motionVectorState{};

    int viewportWidth = 0;
    int viewportHeight = 0;

    std::uintptr_t hdrColorSrv = 0;
    PostProcessTarget* hdrCompositeTarget = nullptr;

    bool pathTracerActive = false;
    PtConvergenceMode pathTracerConvergenceMode = PtConvergenceMode::RealTime;
    void* pathTracerOutputResource = nullptr;
    std::uint32_t pathTracerOutputResourceState = 0;
    std::uintptr_t dxrPathTracerOutputSrv = 0;
    std::uintptr_t dxrReflectionSrv = 0;
    bool pathTracerGridOverlayEnabled = false;

    DlssQuality quality = DlssQuality::DLAA;
    float exposure = 1.0f;
    float dlssSharpness = 0.0f;
    int tonemapMode = 0;

    bool dlssHistoryValid = false;
    bool bloomEnabled = false;
    float bloomThreshold = 1.0f;
    float bloomSoftKnee = 0.5f;
    float bloomBlurRadius = 1.0f;
    float bloomIntensity = 1.0f;
    float bloomTemporalBlendFactor = 0.9f;
    float bloomSameUvBlendFactor = 0.95f;
    float bloomDepthThreshold = 0.01f;
    bool dlssBloomHistoryValid = false;
    int dlssBloomTemporalWarmupFrames = 0;

    bool rayReconstructionActive = false;

    PostProcessTarget* dlssOutputTarget = nullptr;
    PostProcessTarget* ptDlssMotionTarget = nullptr;
    PostProcessTarget* rrDiffuseAlbedoTarget = nullptr;
    PostProcessTarget* rrSpecularAlbedoTarget = nullptr;
    PostProcessTarget* rrNormalRoughnessTarget = nullptr;
    PostProcessTarget* rrSpecularHitDistanceTarget = nullptr;

    PostProcessTarget* dlssBloomExtractTarget = nullptr;
    PostProcessTarget* dlssBloomBlurTarget = nullptr;
    PostProcessTarget* dlssBloomBlur2Target = nullptr;
    PostProcessTarget* dlssBloomHistoryTarget = nullptr;
    PostProcessTarget* dlssBloomTemporalTarget = nullptr;

    Shader* bloomExtractShader = nullptr;
    Shader* bloomBlurShader = nullptr;
    Shader* bloomTemporalShader = nullptr;
    Shader* tonemapShader = nullptr;

    TonemapPassInputs fallbackTonemapInputs{};

    std::function<bool()> patchPathTracerSkyMotion;
    std::function<void()> generateRrGuides;
    std::function<void(PostProcessTarget&, int width, int height)> drawPathTracerGridOverlay;

    // P4b PT RR bundle (devdoc/dxr/pt/full-rr-guides.md). The prepare callback copies the PT
    // bounce-0 material guides into the rr* targets and/or resolves PT depth into
    // ptDlssDepthTarget per ptRrBundleMode, returning ready bits (1 = guides, 2 = depth).
    // ptRrBundleMode: 0 = full PT (all-or-nothing), 1 = raster bundle, 2 = PT guides only,
    // 3 = PT depth+motion, 4 = PT depth only, 5 = PT motion only (2-5 are diagnostic mixes).
    std::function<std::uint32_t()> preparePathTracerRrBundle;
    int ptRrBundleMode = 0;
    PostProcessDepthTarget* ptDlssDepthTarget = nullptr;
    void* pathTracerMotionResource = nullptr;
    std::uint32_t pathTracerMotionResourceState = 0;
    // P4b: PT primary depth (R32) and motion SRVs for bloom temporal when DLSS uses the PT bundle.
    std::uintptr_t pathTracerDepthSrv = 0;
    std::uintptr_t pathTracerMotionSrv = 0;
};

struct DlssResolvePassOutputs
{
    bool dlssRan = false;
    bool pathTracerDlssResolvedThisFrame = false;
    bool pathTracerOutputResourceStateValid = false;
    std::uint32_t pathTracerOutputResourceState = 0;
    bool dlssHistoryValid = false;
    bool dlssBloomHistoryValid = false;
    int dlssBloomTemporalWarmupFrames = 0;
    std::uintptr_t prevFrameBloomSrv = 0;
};

class DlssResolvePass
{
public:
    static void Execute(
        const PostProcessContext& context,
        const DlssResolvePassInputs& inputs,
        DlssResolvePassOutputs& outputs);
};
