#pragma once

#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/post/PostProcessContext.h"

#include <cstdint>

class Framebuffer;
class Shader;

// DXR / RT debug fullscreen blits (HK-C1). Most isolated from Apply()'s main chain.
struct DxrDebugBlitInputs
{
    RenderDebugMode debugMode = RenderDebugMode::None;

    Shader* debugChannelShader = nullptr;
    Shader* dxrPrimaryDebugShader = nullptr;
    Shader* dxrShadowDebugShader = nullptr;
    Shader* rtReflectionResolveShader = nullptr;
    Shader* dxrGiInjectShader = nullptr;

    std::uintptr_t dxrSmokeDebugSrv = 0;
    std::uintptr_t dxrPrimaryOutputSrv = 0;
    std::uintptr_t dxrPrimaryMetadataSrv = 0;
    std::uintptr_t dxrReflectionSrv = 0;
    std::uintptr_t dxrReflectionDenoisedSrv = 0;
    float dxrReflectionUvScaleX = 1.0f;
    float dxrReflectionUvScaleY = 1.0f;
    float dxrReflectionMaxTraceDistance = 100.0f;
    std::uintptr_t dxrShadowPenumbraSrv = 0;
    std::uintptr_t dxrShadowDenoisedSrv = 0;
    float dxrShadowUvScaleX = 1.0f;
    float dxrShadowUvScaleY = 1.0f;
    std::uintptr_t dxrGiRawSrv = 0;
    std::uintptr_t dxrGiDenoisedSrv = 0;
    float dxrGiUvScaleX = 1.0f;
    float dxrGiUvScaleY = 1.0f;
    float dxrGiStrength = 1.0f;

    bool primaryDebugBlitReady = false;
    float maxTraceDistance = 100.0f;

    // Scene MRTs for RtGiInject debug (optional).
    std::uintptr_t sceneIndirectSrv = 0;
    std::uintptr_t sceneDepthSrv = 0;
    std::uintptr_t sceneMaterial0Srv = 0;
    std::uintptr_t sceneMaterial1Srv = 0;
    bool sceneHasMaterialGbuffer = false;
};

class DxrDebugBlitPass
{
public:
    static void BlitDispatchSmoke(
        const PostProcessContext& context,
        const DxrDebugBlitInputs& inputs,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight);

    static void BlitPrimary(
        const PostProcessContext& context,
        const DxrDebugBlitInputs& inputs,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight);

    static void BlitReflection(
        const PostProcessContext& context,
        const DxrDebugBlitInputs& inputs,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight);

    static void BlitShadow(
        const PostProcessContext& context,
        const DxrDebugBlitInputs& inputs,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight);

    static void BlitGi(
        const PostProcessContext& context,
        const DxrDebugBlitInputs& inputs,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight);
};
