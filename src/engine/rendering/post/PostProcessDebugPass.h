#pragma once

#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>
#include <functional>

class Shader;

using CaptureSsaoDiagnosticsFn = std::function<void(
    bool runAo,
    bool compositeRan,
    bool compositeUsesSsao,
    bool pbrDebugActive,
    bool useShadowFactorComposite,
    const char* hdrColorSource,
    const char* ssaoDebugViewSource,
    std::uintptr_t hdrColorSrv,
    std::uintptr_t shadowFactorSrv)>;

struct PostProcessDebugPassInputs
{
    RenderDebugMode debugMode = RenderDebugMode::None;

    bool pbrDebugActive = false;
    bool runAo = false;
    bool runGtao = false;
    bool runRtIndirect = false;
    bool runSsrIndirect = false;
    bool compositeRan = false;
    bool compositeUsesSsao = false;
    bool useShadowFactorComposite = false;

    std::uintptr_t hdrColorSrv = 0;
    std::uintptr_t aoCompositeSrv = 0;
    std::uintptr_t shadowFactorSrv = 0;

    const char* hdrColorSource = "scene_direct";
    const char* ssaoDebugViewSource = "none";

    int ssaoShaderDebugMode = 0;
    float ssgiStrength = 1.0f;
    bool ssgiEnabled = false;
    bool ssgiDenoiseEnabled = false;
    bool ssgiNoiseInjectionEnabled = false;

    std::uintptr_t lastSsrSpatialSrv = 0;
    std::uintptr_t lastSsrTemporalSrv = 0;
    std::uintptr_t lastSsrVarianceSrv = 0;
    std::uintptr_t lastSsrDenoiseSrv = 0;
    std::uintptr_t lastSsrResolvedSrv = 0;
    std::uintptr_t lastSsgiInjectSrv = 0;

    Framebuffer* sceneFramebuffer = nullptr;

    PostProcessTarget* ssaoTarget = nullptr;
    PostProcessTarget* gtaoRawTarget = nullptr;
    PostProcessTarget* hdrCompositeTarget = nullptr;
    PostProcessTarget* radianceTarget = nullptr;
    PostProcessTarget* radianceTraceInputTarget = nullptr;
    PostProcessTarget* radianceSpatialTarget = nullptr;
    PostProcessTarget* radianceHistoryTarget = nullptr;
    PostProcessTarget* ssrSceneColorTarget = nullptr;
    PostProcessTarget* ssrTraceTarget = nullptr;
    PostProcessTarget* ssrIndirectTarget = nullptr;
    PostProcessTarget* rtIndirectTarget = nullptr;
    PostProcessTarget* ptTemporalStatsTarget = nullptr;

    std::uintptr_t ptCurrentRadianceSrv = 0;
    std::uintptr_t ptPreviousRadianceSrv = 0;
    std::uintptr_t ptCurrentDepthSrv = 0;
    std::uintptr_t ptPreviousDepthSrv = 0;
    std::uintptr_t ptMotionSrv = 0;
    bool ptPreviousRadianceValid = false;

    Shader* debugChannelShader = nullptr;
    Shader* velocityDebugShader = nullptr;
    Shader* gbufferDebugShader = nullptr;
    Shader* radianceDebugShader = nullptr;
    Shader* ssrDebugShader = nullptr;
    Shader* ssrTraceDebugShader = nullptr;
    Shader* ssrDenoiseDebugShader = nullptr;
    Shader* ssgiDenoiseDebugShader = nullptr;
    Shader* giTemporalDebugShader = nullptr;
    Shader* ptTemporalStatsDebugShader = nullptr;
    Shader* ptMotionReprojectionDebugShader = nullptr;

    CaptureSsaoDiagnosticsFn captureSsaoDiagnostics;
    bool logSsaoApplySnapshot = false;
};

struct PostProcessDebugPassOutputs
{
    bool earlyOut = false;
    bool requestGpuReadback = false;
    const char* ssaoDebugViewSource = "none";
};

class PostProcessDebugPass
{
public:
    static bool TryExecute(
        const PostProcessContext& context,
        const PostProcessDebugPassInputs& inputs,
        PostProcessDebugPassOutputs& outputs);
};
