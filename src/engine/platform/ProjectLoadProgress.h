#pragma once

#include "engine/platform/NativeProgressWindow.h"

#include <algorithm>

// A single, benchmark-calibrated presentation profile for project opening.
//
// The first profile is based on the d12test8 cold-process baseline captured on 2026-07-14
// (about 8.1 s median): opening the project is about 9%, GPU resource initialization plus
// environment work about 13%, DXR pipeline warm-up about 46%, and recording the first Scene
// View frame consumes most of the remainder. Update these anchors when the benchmark changes;
// callers should only use the named stages below rather than embedding display percentages.
// This is intentionally a presentation estimate, not a stopwatch: project content and enabled
// renderer features can change the exact duration of an individual stage.
namespace ProjectLoadProgress
{
    constexpr float kOpeningProject = 0.01f;
    constexpr float kFinishingPreviousGpuWork = 0.01f;
    constexpr float kReadingProjectFile = 0.015f;
    constexpr float kDeserializingSceneStart = 0.02f;
    constexpr float kDeserializingSceneEnd = 0.06f;
    constexpr float kProjectOpened = 0.09f;
    constexpr float kEditorReady = 0.10f;

    constexpr float kGpuInitializationStart = 0.11f;
    constexpr float kGpuInitializationEnd = 0.19f;
    constexpr float kEnvironmentSync = 0.20f;
    constexpr float kIblCaptureTargets = 0.205f;
    constexpr float kIblHdrLoad = 0.210f;
    constexpr float kIblCubemap = 0.217f;
    constexpr float kIblPrefilter = 0.224f;
    constexpr float kIblBrdfLut = 0.230f;

    constexpr float kDxrWarmupStart = 0.24f;
    constexpr float kDxrWarmupEnd = 0.68f;

    constexpr float kFirstSceneFrameStart = 0.69f;
    constexpr float kSceneUpload = 0.71f;
    constexpr float kSceneLighting = 0.73f;
    constexpr float kSceneShadows = 0.75f;
    constexpr float kSceneRaster = 0.79f;
    constexpr float kScenePostProcess = 0.83f;
    constexpr float kDxrAccelerationStructures = 0.86f;
    constexpr float kDxrDispatch = 0.90f;
    constexpr float kGameViewFirstFrame = 0.92f;
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

    inline void Report(const std::string& message, const float progress)
    {
        NativeProgressWindow::Instance().Report(message, progress);
    }

    inline void SetProgress(const float progress)
    {
        NativeProgressWindow::Instance().SetProgress(progress);
    }
}
