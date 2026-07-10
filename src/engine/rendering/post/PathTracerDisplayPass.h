#pragma once

#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/glm.hpp>

class Camera;
class Framebuffer;
class Shader;

// Inputs that must stay stable for progressive PT reference accumulation (P3).
struct PathTracerHistoryKey
{
    glm::mat4 viewProjection{1.0f};
    int width = 0;
    int height = 0;
    PtConvergenceMode convergenceMode = PtConvergenceMode::RealTime;
    float maxTraceDistance = 100.0f;
    glm::vec3 sunDirection{0.0f, -1.0f, 0.0f};
    glm::vec3 sunColor{1.0f};
    float sunIntensity = 0.0f;
    float environmentIntensity = 1.0f;
    std::size_t geometryObjectCount = 0;
    std::uint32_t sceneVersion = 0;

    bool operator==(const PathTracerHistoryKey& other) const;
};

struct PathTracerAccumulateInputs
{
    PathTracerHistoryKey historyKey{};
    PathTracerHistoryKey currentHistoryKey{};
    std::uint32_t sampleCount = 0;
    bool pingPongReadFromScratch = false;

    std::uintptr_t currentFrameSrv = 0;
    int width = 0;
    int height = 0;

    Shader* accumulateShader = nullptr;
    PostProcessTarget* sumTarget = nullptr;
    PostProcessTarget* scratchTarget = nullptr;
};

struct PathTracerAccumulateOutputs
{
    PathTracerHistoryKey historyKey{};
    std::uint32_t sampleCount = 0;
    bool pingPongReadFromScratch = false;
    std::uintptr_t sumDisplaySrv = 0;
};

struct PathTracerBlitInputs
{
    bool pathTracerActive = false;
    bool pathTracerBlitReady = false;
    bool pathTracerPostIntegrated = false;
    bool pathTracerDlssResolvedThisFrame = false;

    PtConvergenceMode convergenceMode = PtConvergenceMode::RealTime;
    std::uint32_t accumSampleCount = 0;
    std::uintptr_t accumSumDisplaySrv = 0;
    std::uintptr_t pathTracerOutputSrv = 0;
    std::uintptr_t pathTracerMetadataSrv = 0;
    float maxTraceDistance = 100.0f;

    const Framebuffer* outputTarget = nullptr;
    int viewportWidth = 0;
    int viewportHeight = 0;

    Shader* primaryDebugShader = nullptr;
};

struct PathTracerHdrCopyInputs
{
    PtConvergenceMode convergenceMode = PtConvergenceMode::RealTime;
    std::uint32_t accumSampleCount = 0;
    std::uintptr_t accumSumDisplaySrv = 0;
    std::uintptr_t pathTracerOutputSrv = 0;

    Shader* meanShader = nullptr;
    Shader* downsampleShader = nullptr;
    PostProcessTarget* hdrCompositeTarget = nullptr;
};

struct PathTracerIntegrateInputs
{
    bool pathTracerActive = false;
    std::uintptr_t pathTracerOutputSrv = 0;
    bool gridOverlayEnabled = false;

    PathTracerHdrCopyInputs hdrCopy{};
    const Camera* camera = nullptr;

    std::function<void(PostProcessTarget&, int width, int height)> drawGridOverlay;
};

struct PathTracerIntegrateOutputs
{
    bool integrated = false;
    std::uintptr_t hdrColorSrv = 0;
};

struct PathTracerGridOverlayInputs
{
    const Camera* camera = nullptr;
    PostProcessTarget* target = nullptr;
    int width = 0;
    int height = 0;

    int renderWidth = 0;
    int renderHeight = 0;
    Framebuffer* sceneFramebuffer = nullptr;

    PostProcessDepthTarget* dlssDisplayDepthTarget = nullptr;
    Shader* depthBlitShader = nullptr;

    std::function<void(const Camera&, bool useDepthTest)> gridOverlayDraw;
};

struct PathTracerDlssDepthResolveInputs
{
    std::uintptr_t pathTracerDepthSrv = 0;
    PostProcessDepthTarget* ptDlssDepthTarget = nullptr;
    Shader* depthBlitShader = nullptr;
    int renderWidth = 0;
    int renderHeight = 0;
};

struct PathTracerSkyMotionPatchInputs
{
    std::uintptr_t pathTracerMetadataSrv = 0;
    std::uintptr_t pathTracerMotionSrv = 0;
    Framebuffer* sceneFramebuffer = nullptr;
    PostProcessTarget* ptDlssMotionTarget = nullptr;
    Shader* skyMotionPatchShader = nullptr;
};

class PathTracerDisplayPass
{
public:
    static void AccumulateReference(
        const PostProcessContext& context,
        const PathTracerAccumulateInputs& inputs,
        PathTracerAccumulateOutputs& outputs);

    static void Blit(
        const PostProcessContext& context,
        const PathTracerBlitInputs& inputs);

    static void CopyHdrToCompositeTarget(
        const PostProcessContext& context,
        const PathTracerHdrCopyInputs& inputs,
        const float clearColor[4]);

    static void PrepareDlssHdrInput(
        const PostProcessContext& context,
        const PathTracerHdrCopyInputs& inputs);

    static void IntegrateIntoHdrChain(
        const PostProcessContext& context,
        const PathTracerIntegrateInputs& inputs,
        PathTracerIntegrateOutputs& outputs);

    static void DrawGridOverlay(
        const PostProcessContext& context,
        const PathTracerGridOverlayInputs& inputs);

    static bool ResolveDlssDepth(
        const PostProcessContext& context,
        const PathTracerDlssDepthResolveInputs& inputs);

    static bool PatchSkyMotion(
        const PostProcessContext& context,
        const PathTracerSkyMotionPatchInputs& inputs);
};
