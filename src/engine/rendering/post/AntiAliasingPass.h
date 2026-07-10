#pragma once

#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Framebuffer;
class Shader;

struct TaaPassInputs
{
    bool useTaa = false;

    std::uintptr_t hdrColorSrv = 0;
    glm::vec2 texelSize{0.0f};

    glm::mat4 viewMatrix{1.0f};
    glm::mat4 unjitteredProjectionMatrix{1.0f};
    MotionVectorFrameState motionVectorState{};

    float taaBlendFactor = 0.1f;
    bool taaHistoryValid = false;

    Framebuffer* sceneFramebuffer = nullptr;
    Shader* taaShader = nullptr;
    PostProcessTarget* taaHistoryTarget = nullptr;
    PostProcessTarget* taaResolveTarget = nullptr;
};

struct TaaPassOutputs
{
    bool ran = false;
    std::uintptr_t hdrColorSrv = 0;
    bool taaHistoryValid = false;
};

struct LdrAntiAliasingInputs
{
    bool useFxaa = false;
    bool useSmaa = false;
    bool useSsaa = false;

    int viewportWidth = 0;
    int viewportHeight = 0;
    glm::vec2 texelSize{0.0f};

    std::uintptr_t ldrTonemapSrv = 0;

    float fxaaSubpixQuality = 0.75f;
    float fxaaEdgeThreshold = 0.166f;
    float smaaThreshold = 0.1f;
    int smaaSearchSteps = 4;

    Shader* fxaaShader = nullptr;
    Shader* smaaEdgeShader = nullptr;
    Shader* smaaNeighborShader = nullptr;
    Shader* downsampleShader = nullptr;

    PostProcessTarget* smaaEdgeTarget = nullptr;
    PostProcessTarget* smaaOutputTarget = nullptr;

    const Framebuffer* outputTarget = nullptr;
};

struct MsaaDepthResolveInputs
{
    Framebuffer* sceneFramebuffer = nullptr;
    Shader* msaaDepthResolveShader = nullptr;
};

class AntiAliasingPass
{
public:
    static bool NeedsLdrIntermediate(
        bool useFxaa,
        bool useSmaa,
        bool useSsaa,
        int renderWidth,
        int renderHeight,
        int storedViewportWidth,
        int storedViewportHeight);

    static void ExecuteTaa(
        const PostProcessContext& context,
        const TaaPassInputs& inputs,
        TaaPassOutputs& outputs);

    static void ExecuteLdrAntiAliasing(
        const PostProcessContext& context,
        const LdrAntiAliasingInputs& inputs);

    static void ExecuteMsaaDepthResolve(
        const PostProcessContext& context,
        const MsaaDepthResolveInputs& inputs);
};
