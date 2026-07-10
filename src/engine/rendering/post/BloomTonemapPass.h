#pragma once

#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Framebuffer;
class Shader;

struct RenderResBloomInputs
{
    std::uintptr_t hdrColorSrv = 0;
    glm::vec2 fullTexelSize{0.0f};

    float exposure = 1.0f;
    float bloomThreshold = 1.0f;
    float bloomSoftKnee = 0.5f;
    float bloomBlurRadius = 1.0f;
    float bloomTemporalBlendFactor = 0.9f;
    float bloomSameUvBlendFactor = 0.95f;
    float bloomDepthThreshold = 0.01f;

    bool useMaterialGbuffer = false;
    std::uintptr_t material0Srv = 0;
    std::uintptr_t material1Srv = 0;
    std::uintptr_t velocitySrv = 0;
    std::uintptr_t depthSrv = 0;
    bool hasVelocity = false;

    Shader* bloomExtractShader = nullptr;
    Shader* bloomBlurShader = nullptr;
    Shader* bloomTemporalShader = nullptr;

    PostProcessTarget* bloomExtractTarget = nullptr;
    PostProcessTarget* bloomBlurTarget = nullptr;
    PostProcessTarget* bloomBlur2Target = nullptr;
    PostProcessTarget* bloomTemporalTarget = nullptr;
    PostProcessTarget* bloomHistoryTarget = nullptr;

    bool bloomHistoryValid = false;
    int bloomTemporalWarmupFrames = 0;
};

struct RenderResBloomOutputs
{
    std::uintptr_t bloomSrv = 0;
    bool bloomHistoryValid = false;
    int bloomTemporalWarmupFrames = 0;
};

struct TonemapPassInputs
{
    std::uintptr_t hdrColorSrv = 0;
    std::uintptr_t bloomSrv = 0;
    glm::vec2 texelSize{0.0f};

    float exposure = 1.0f;
    int tonemapMode = 0;
    bool bloomEnabled = false;
    float bloomIntensity = 1.0f;

    Shader* tonemapShader = nullptr;
    PostProcessTarget* ldrTonemapTarget = nullptr;
};

struct DisplayResBloomInputs
{
    std::uintptr_t hdrColorSrv = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    int renderWidth = 0;
    int renderHeight = 0;

    float exposure = 1.0f;
    float bloomThreshold = 1.0f;
    float bloomSoftKnee = 0.5f;
    float bloomBlurRadius = 1.0f;
    float bloomTemporalBlendFactor = 0.9f;
    float bloomSameUvBlendFactor = 0.95f;
    float bloomDepthThreshold = 0.01f;

    bool useMaterialGbuffer = false;
    std::uintptr_t material0Srv = 0;
    std::uintptr_t material1Srv = 0;
    std::uintptr_t velocitySrv = 0;
    std::uintptr_t depthSrv = 0;
    bool hasVelocity = false;

    Shader* bloomExtractShader = nullptr;
    Shader* bloomBlurShader = nullptr;
    Shader* bloomTemporalShader = nullptr;

    PostProcessTarget* bloomExtractTarget = nullptr;
    PostProcessTarget* bloomBlurTarget = nullptr;
    PostProcessTarget* bloomBlur2Target = nullptr;
    PostProcessTarget* bloomTemporalTarget = nullptr;
    PostProcessTarget* bloomHistoryTarget = nullptr;

    bool bloomHistoryValid = false;
    int bloomTemporalWarmupFrames = 0;
};

struct DisplayResBloomOutputs
{
    std::uintptr_t bloomSrv = 0;
    bool bloomHistoryValid = false;
    int bloomTemporalWarmupFrames = 0;
};

class BloomTonemapPass
{
public:
    static bool ExecuteRenderResBloom(
        const PostProcessContext& context,
        const RenderResBloomInputs& inputs,
        RenderResBloomOutputs& outputs);

    static bool ExecuteDisplayResBloom(
        const PostProcessContext& context,
        const DisplayResBloomInputs& inputs,
        DisplayResBloomOutputs& outputs);

    static void ExecuteTonemapToLdrTarget(
        const PostProcessContext& context,
        const TonemapPassInputs& inputs);

    static void ExecuteTonemapToViewport(
        const PostProcessContext& context,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight,
        const TonemapPassInputs& inputs);

    static void ExecuteTonemapDlssDisplay(
        const PostProcessContext& context,
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight,
        std::uintptr_t dlssOutputSrv,
        std::uintptr_t displayBloomSrv,
        float exposure,
        int tonemapMode,
        float bloomIntensity,
        Shader* tonemapShader);
};
