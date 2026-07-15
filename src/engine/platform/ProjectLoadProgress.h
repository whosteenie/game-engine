#pragma once

#include "engine/platform/NativeProgressWindow.h"

#include <algorithm>

// A single, benchmark-calibrated presentation profile for project opening.
//
// This profile is based on the d12test8 cold-process benchmark captured on 2026-07-14 after
// parallel DXR and screen-space stage prewarming (about 3.6 s median): project opening is about
// 22%, GPU initialization plus environment work about 9%, DXR warm-up about 37%, and the first
// Scene View frame consumes most of the remainder. Update these anchors when the benchmark changes;
// callers should only use the named stages below rather than embedding display percentages.
// This is intentionally a presentation estimate, not a stopwatch: project content and enabled
// renderer features can change the exact duration of an individual stage.
namespace ProjectLoadProgress
{
    constexpr float kOpeningProject = 0.01f;
    constexpr float kFinishingPreviousGpuWork = 0.01f;
    constexpr float kReadingProjectFile = 0.015f;
    constexpr float kDeserializingSceneStart = 0.02f;
    constexpr float kDeserializingSceneEnd = 0.20f;
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
    constexpr float kDxrWarmupEnd = 0.70f;

    constexpr float kFirstSceneFrameStart = 0.71f;
    constexpr float kSceneUpload = 0.88f;
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

    inline void Report(const std::string& message, const float progress)
    {
        NativeProgressWindow::Instance().Report(message, progress);
    }

    inline void SetProgress(const float progress)
    {
        NativeProgressWindow::Instance().SetProgress(progress);
    }
}
