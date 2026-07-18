#pragma once

#include "engine/platform/tooling/NativeProgressWindow.h"

#include <algorithm>

// A single, ordered presentation profile for project opening. Callers must use these milestones
// rather than embedding display percentages. The bar is an ETA-style UX indicator, not a
// stopwatch, but its ranges follow the real dependency order: file I/O and scene deserialization,
// imported-model prewarm, editor setup, GPU setup, environment work, DXR warm-up, then the first
// frame. No stage may report into a later stage's range.
namespace ProjectLoadProgress
{
    constexpr float kOpeningProject = 0.01f;
    constexpr float kFinishingPreviousGpuWork = 0.01f;
    constexpr float kReadingProjectFile = 0.02f;
    constexpr float kParsingProjectFile = 0.04f;
    constexpr float kDeserializingSceneStart = 0.05f;
    constexpr float kDeserializingSceneEnd = 0.18f;
    constexpr float kPrewarmingProjectModelsStart = kDeserializingSceneEnd;
    constexpr float kPrewarmingProjectModelsEnd = 0.22f;
    constexpr float kProjectOpened = 0.22f;
    constexpr float kEditorReady = 0.23f;

    constexpr float kGpuInitializationStart = 0.24f;
    constexpr float kGpuInitializationEnd = 0.30f;
    constexpr float kEnvironmentSync = 0.31f;
    constexpr float kIblCaptureTargets = 0.312f;
    constexpr float kIblHdrLoad = 0.320f;
    constexpr float kIblCubemap = 0.330f;
    constexpr float kIblPrefilter = 0.340f;
    constexpr float kIblBrdfLut = 0.345f;

    constexpr float kDxrWarmupStart = 0.35f;
    // CPU shader-library compilation and device RTPSO creation are distinct, measured pieces of
    // the DXR warm-up. Keep their presentation ranges separate so the native progress window can
    // advance when parallel compile jobs actually complete instead of holding at one value.
    constexpr float kDxrShaderLibraryWarmupEnd = 0.62f;
    constexpr float kDxrPipelineWarmupStart = kDxrShaderLibraryWarmupEnd;
    constexpr float kDxrWarmupEnd = 0.70f;

    constexpr float kFirstSceneFrameStart = 0.71f;
    constexpr float kSceneGpuTableBuildStart = 0.72f;
    constexpr float kSceneUpload = 0.86f;
    constexpr float kSceneLighting = 0.885f;
    constexpr float kSceneShadows = 0.89f;
    constexpr float kSceneRaster = 0.90f;
    constexpr float kScenePostProcess = 0.91f;
    constexpr float kDxrAccelerationStructures = 0.94f;
    constexpr float kDxrDispatch = 0.95f;
    constexpr float kGameViewFirstFrame = 0.96f;
    constexpr float kSceneComposite = 0.96f;
    constexpr float kGameViewComposite = 0.97f;

    inline float Lerp(const float start, const float end, const float fraction)
    {
        return start + (end - start) * std::clamp(fraction, 0.0f, 1.0f);
    }

    inline float SceneDeserialization(const float fraction)
    {
        return Lerp(kDeserializingSceneStart, kDeserializingSceneEnd, fraction);
    }

    inline float GpuInitialization(const float fraction)
    {
        return Lerp(kGpuInitializationStart, kGpuInitializationEnd, fraction);
    }

    inline float DxrWarmup(const float fraction)
    {
        return Lerp(kDxrWarmupStart, kDxrWarmupEnd, fraction);
    }

    inline float DxrShaderLibraryWarmup(const float fraction)
    {
        return Lerp(kDxrWarmupStart, kDxrShaderLibraryWarmupEnd, fraction);
    }

    inline float DxrPipelineWarmup(const float fraction)
    {
        return Lerp(kDxrPipelineWarmupStart, kDxrWarmupEnd, fraction);
    }

    inline void Report(const std::string& message, const float progress)
    {
        NativeProgressWindow::Instance().Report(message, progress);
    }

    inline void SetProgress(const float progress)
    {
        NativeProgressWindow::Instance().SetProgress(progress);
    }
}
