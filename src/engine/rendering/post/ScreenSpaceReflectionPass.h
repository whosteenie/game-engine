#pragma once

#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Framebuffer;
class Shader;

struct ScreenSpaceReflectionPassInputs
{
    bool pbrDebugActive = false;
    bool useShadowFactorComposite = false;
    bool rtCompositeWanted = false;

    bool ssrEnabled = false;
    bool wantsSsr = false;
    bool isSsrTraceDebug = false;
    bool isSsrDenoiseDebug = false;
    bool isSsrCompositeDebug = false;
    bool ssrSpecReplacementDebug = false;

    bool sceneHasSplitLighting = false;
    bool sceneHasShadowFactor = false;
    bool sceneHasGeometryNormals = false;
    bool sceneHasMaterialGbuffer = false;
    bool sceneHasVelocity = false;

    glm::mat4 projectionMatrix{1.0f};
    glm::mat4 inverseProjectionMatrix{1.0f};
    glm::mat4 unjitteredProjectionMatrix{1.0f};
    glm::mat4 viewMatrix{1.0f};
    glm::vec2 texelSize{0.0f};

    MotionVectorFrameState motionVectorState{};

    std::uintptr_t shadowFactorSrv = 0;
    std::uintptr_t directLightingSrv = 0;
    std::uintptr_t indirectLightingSrv = 0;
    std::uintptr_t depthSrv = 0;
    std::uintptr_t normalSrv = 0;
    std::uintptr_t material0Srv = 0;
    std::uintptr_t material1Srv = 0;
    std::uintptr_t velocitySrv = 0;

    bool bloomEnabled = false;
    float bloomIntensity = 1.0f;
    std::uintptr_t prevFrameBloomSrv = 0;

    float ssrMaxTraceDistance = 100.0f;
    int ssrStepCount = 32;
    float ssrThickness = 0.5f;
    float ssrRoughnessCutoff = 0.5f;
    float ssrStepExponent = 1.0f;
    int ssrSampleCount = 1;
    bool ssrDenoiseEnabled = false;
    float ssrTraceResolutionScale = 1.0f;
    float ssrTemporalBlendFactor = 0.9f;
    float ssrSameUvBlendFactor = 0.95f;
    float ssrDepthThreshold = 0.01f;
    float ssrSpatialDepthThreshold = 0.01f;
    float ssrSpatialBlurSpread = 1.0f;
    float ssrRoughnessSpreadMin = 0.75f;
    float ssrRoughnessSpreadMax = 1.75f;
    float ssrSvgfPhiEpsilon = 1e-4f;
    float ssrSvgfFilterStrength = 1.0f;
    float ssrStrength = 1.0f;

    int ssrFrameIndex = 0;
    bool ssrHistoryValid = false;

    bool iblReady = false;
    float environmentIntensity = 1.0f;
    float maxReflectionLod = 0.0f;
    std::uintptr_t prefilterMapSrv = 0;
    std::uintptr_t brdfLutSrv = 0;

    Framebuffer* sceneFramebuffer = nullptr;

    Shader* ssrSceneColorShader = nullptr;
    Shader* ssrTraceShader = nullptr;
    Shader* ssrSvgfTemporalShader = nullptr;
    Shader* temporalReprojectShader = nullptr;
    Shader* giDepthHistoryShader = nullptr;
    Shader* ssrSvgfVarianceTemporalShader = nullptr;
    Shader* ssrSvgfAtrousShader = nullptr;
    Shader* ssrUpscaleShader = nullptr;
    Shader* ssrIndirectShader = nullptr;

    PostProcessTarget* ssrSceneColorTarget = nullptr;
    PostProcessTarget* ssrTraceTarget = nullptr;
    PostProcessTarget* ssrSpatialTarget = nullptr;
    PostProcessTarget* ssrSpatialBlurTarget = nullptr;
    PostProcessTarget* ssrHistoryTarget = nullptr;
    PostProcessTarget* ssrTemporalTarget = nullptr;
    PostProcessTarget* ssrVarianceHistoryTarget = nullptr;
    PostProcessTarget* ssrVarianceTemporalTarget = nullptr;
    PostProcessTarget* ssrHistoryDepthTarget = nullptr;
    PostProcessTarget* ssrResolvedTarget = nullptr;
    PostProcessTarget* ssrIndirectTarget = nullptr;
};

struct ScreenSpaceReflectionPassOutputs
{
    std::uintptr_t indirectCompositeSrv = 0;

    bool ssrSceneColorRanLastFrame = false;
    bool ssrTraceRanLastFrame = false;
    bool ssrDenoiseRanLastFrame = false;
    bool ssrTemporalRanLastFrame = false;

    std::uintptr_t lastSsrSpatialSrv = 0;
    std::uintptr_t lastSsrTemporalSrv = 0;
    std::uintptr_t lastSsrVarianceSrv = 0;
    std::uintptr_t lastSsrDenoiseSrv = 0;
    std::uintptr_t lastSsrResolvedSrv = 0;

    int ssrFrameIndex = 0;
    bool ssrHistoryValid = false;

    bool ssrIndirectRan = false;
};

class ScreenSpaceReflectionPass
{
public:
    static void Execute(
        const PostProcessContext& context,
        const ScreenSpaceReflectionPassInputs& inputs,
        ScreenSpaceReflectionPassOutputs& outputs);
};
