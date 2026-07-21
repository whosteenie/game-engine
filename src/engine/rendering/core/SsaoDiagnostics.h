#pragma once

#include "engine/rendering/core/RenderDebug.h"

#include <cstdint>

// Live SSAO pipeline snapshot for the lighting panel and opt-in stderr logs.
struct SsaoDiagnosticsSnapshot
{
    std::uint64_t captureFrame = 0;
    bool enabled = false;
    bool postProcessEnabled = false;
    bool passExecuted = false;
    bool compositeUsesSsao = false;
    bool compositeRan = false;
    bool shadowComposite = false;
    bool splitLighting = false;
    bool geometryNormals = false;
    bool pbrDebugActive = false;
    RenderDebugMode debugMode = RenderDebugMode::None;

    int sceneWidth = 0;
    int sceneHeight = 0;

    std::uintptr_t depthSrv = 0;
    std::uintptr_t normalSrv = 0;
    std::uintptr_t noiseSrv = 0;
    std::uintptr_t ssaoRawSrv = 0;
    std::uintptr_t ssaoBlurSrv = 0;
    std::uintptr_t hdrColorSrv = 0;
    std::uintptr_t shadowFactorSrv = 0;

    bool hasUniformSamples = false;
    bool hasUniformKernelSize = false;
    int kernelCount = 0;
    float kernelSample0X = 0.0f;
    float kernelSample0Y = 0.0f;
    float kernelSample0Z = 0.0f;
    float radius = 0.0f;
    float bias = 0.0f;
    float aoStrength = 0.0f;
    float ssaoPower = 0.0f;

    const char* hdrColorSource = "unknown";
    const char* ssaoDebugViewSource = "none";

    // Populated on the frame after a toggle/readback request (GPU completed).
    bool gpuReadbackValid = false;
    float centerDepth = -1.0f;
    float centerNormalR = -1.0f;
    float centerNormalG = -1.0f;
    float centerNormalB = -1.0f;
    float centerSsaoRaw = -1.0f;
    float centerSsaoBlur = -1.0f;
    float centerHardwareDepth = -1.0f;
};
