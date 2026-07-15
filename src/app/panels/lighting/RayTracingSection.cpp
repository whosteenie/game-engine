#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/editor/RendererSettingUi.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/EnvironmentPresets.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/DxrCapabilities.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/assets/FileDialog.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
    constexpr float kP7DiagnosticOrbitDurationSeconds = 10.0f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr std::uint32_t kP7DiagnosticBatchWarmupFrames = 60u;
    constexpr std::uint32_t kP7DiagnosticBatchStaticFrames = 600u;
    constexpr std::uint32_t kP7DiagnosticBatchOrbitFrames = 600u;

    struct P7DiagnosticOrbitState
    {
        bool active = false;
        bool p7Enabled = false;
        float elapsedSeconds = 0.0f;
        float durationSeconds = kP7DiagnosticOrbitDurationSeconds;
        int revolutions = 3;
        float horizontalRadius = 0.0f;
        float startAngle = 0.0f;
        float startYaw = 0.0f;
        float startPitch = 0.0f;
        glm::vec3 target{0.0f};
        glm::vec3 startPosition{0.0f};
        bool fixedFrameStep = false;
        bool preFinalReadbackDrain = false;
        bool finalReadbackDrain = false;
        std::uint32_t targetFrames = 0;
        std::uint32_t completedFrames = 0;
        float measurementWallSeconds = 0.0f;
        std::string status;
    };

    enum class P7DiagnosticBatchStage
    {
        Idle,
        P7OnWarmup,
        P7OnStatic,
        P7OnOrbit,
        P7OffWarmup,
        P7OffStatic,
        P7OffOrbit,
    };

    struct P7DiagnosticBatchState
    {
        bool active = false;
        P7DiagnosticBatchStage stage = P7DiagnosticBatchStage::Idle;
        float stageMeasurementWallSeconds = 0.0f;
        float p7OnStaticWallSeconds = 0.0f;
        float p7OffStaticWallSeconds = 0.0f;
        bool stagePreFinalReadbackDrain = false;
        bool stageFinalReadbackDrain = false;
        int revolutions = 3;
        bool originalP5Enabled = false;
        bool originalP6Enabled = false;
        bool originalP7Enabled = false;
        RenderDebugMode originalDebugMode = RenderDebugMode::None;
        glm::vec3 originalCameraPosition{0.0f};
        float originalCameraYaw = 0.0f;
        float originalCameraPitch = 0.0f;
        glm::vec3 captureCameraPosition{0.0f};
        float captureCameraYaw = 0.0f;
        float captureCameraPitch = 0.0f;
        glm::vec3 target{0.0f};
        std::string status;
    };

    P7DiagnosticOrbitState g_p7DiagnosticOrbit;
    P7DiagnosticBatchState g_p7DiagnosticBatch;

    bool ComputePrimarySelectionFocus(
        const Scene& scene,
        glm::vec3& outTarget,
        float& outRadius)
    {
        const int selectedIndex = scene.GetPrimarySelection();
        if (selectedIndex < 0
            || static_cast<std::size_t>(selectedIndex) >= scene.GetObjects().size())
        {
            return false;
        }

        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        scene.GetWorldBounds(selectedIndex, boundsMin, boundsMax);
        outTarget = 0.5f * (boundsMin + boundsMax);
        outRadius = std::max(glm::length(0.5f * (boundsMax - boundsMin)), 0.05f);
        return true;
    }

    bool PointCameraYawAtTarget(Camera& camera, const glm::vec3& target)
    {
        const glm::vec3 position = camera.GetPosition();
        const glm::vec2 horizontalDirection(target.x - position.x, target.z - position.z);
        if (glm::dot(horizontalDirection, horizontalDirection) <= 1.0e-6f)
        {
            return false;
        }

        const float yaw = glm::degrees(std::atan2(horizontalDirection.y, horizontalDirection.x));
        camera.SetOrientation(yaw, camera.GetPitch());
        return true;
    }

    void RestoreP7DiagnosticOrbitStart(Camera& camera, P7DiagnosticOrbitState& orbit)
    {
        camera.SetPosition(orbit.startPosition);
        camera.SetOrientation(orbit.startYaw, orbit.startPitch);
    }

    void SetCameraPose(
        Camera& camera,
        const glm::vec3& position,
        const float yaw,
        const float pitch)
    {
        camera.SetPosition(position);
        camera.SetOrientation(yaw, pitch);
    }

    bool BeginP7DiagnosticOrbit(
        SceneRenderer& renderer,
        Camera& camera,
        const glm::vec3& target,
        const int revolutions,
        const std::uint32_t fixedFrameCount,
        P7DiagnosticOrbitState& orbit,
        std::string& outStatus)
    {
        const glm::vec3 startPosition = camera.GetPosition();
        const glm::vec3 offset = startPosition - target;
        const float horizontalRadius = glm::length(glm::vec2(offset.x, offset.z));
        if (horizontalRadius < 0.1f)
        {
            outStatus = "Camera is too close to directly above/below the selection. Frame it first.";
            return false;
        }

        orbit.active = true;
        orbit.p7Enabled = renderer.GetDxrSettings().IsRestirGiSpatialEnabled();
        orbit.elapsedSeconds = 0.0f;
        orbit.durationSeconds = kP7DiagnosticOrbitDurationSeconds;
        orbit.revolutions = std::clamp(revolutions, 1, 20);
        orbit.horizontalRadius = horizontalRadius;
        orbit.startAngle = std::atan2(offset.z, offset.x);
        orbit.target = target;
        orbit.startPosition = startPosition;
        orbit.fixedFrameStep = fixedFrameCount > 0u;
        orbit.preFinalReadbackDrain = false;
        orbit.finalReadbackDrain = false;
        orbit.targetFrames = fixedFrameCount;
        orbit.completedFrames = 0u;
        orbit.measurementWallSeconds = 0.0f;
        PointCameraYawAtTarget(camera, target);
        orbit.startYaw = camera.GetYaw();
        orbit.startPitch = camera.GetPitch();
        renderer.SetRenderDebugMode(RenderDebugMode::PtRestirGiSpatialMotionDelta);
        renderer.GetScreenSpaceEffects().ResetPathTracerTemporalDiagnostics();
        outStatus = std::string(orbit.fixedFrameStep
                ? "Recording deterministic P7 "
                : "Recording repeatable P7 ")
            + (orbit.p7Enabled ? "ON" : "OFF")
            + " orbit (" + std::to_string(orbit.revolutions) + " revolutions / "
            + (orbit.fixedFrameStep
                ? std::to_string(orbit.targetFrames) + " rendered frames)..."
                : "10 s)...");
        orbit.status = outStatus;
        return true;
    }

    void ConfigureP7DiagnosticStaticStage(
        SceneRenderer& renderer,
        Camera& camera,
        P7DiagnosticBatchState& batch,
        const bool p7Enabled)
    {
        DxrSettings& settings = renderer.GetDxrSettings();
        settings.SetRestirGiInitialEnabled(true);
        settings.SetRestirGiTemporalEnabled(true);
        settings.SetRestirGiSpatialEnabled(p7Enabled);
        SetCameraPose(
            camera,
            batch.captureCameraPosition,
            batch.captureCameraYaw,
            batch.captureCameraPitch);
        renderer.SetRenderDebugMode(RenderDebugMode::PtRestirGiSpatialStaticVariance);
        renderer.ResetPathTracerRestirDiagnosticState();
        batch.stageMeasurementWallSeconds = 0.0f;
        batch.stagePreFinalReadbackDrain = false;
        batch.stageFinalReadbackDrain = false;
    }

    void RestoreP7DiagnosticBatch(
        SceneRenderer& renderer,
        Camera& camera,
        P7DiagnosticBatchState& batch)
    {
        DxrSettings& settings = renderer.GetDxrSettings();
        settings.SetRestirGiInitialEnabled(batch.originalP5Enabled);
        settings.SetRestirGiTemporalEnabled(batch.originalP6Enabled);
        settings.SetRestirGiSpatialEnabled(batch.originalP7Enabled);
        renderer.GetScreenSpaceEffects().SetPathTracerTemporalDiagnosticsPaused(false);
        renderer.SetRenderDebugMode(batch.originalDebugMode);
        SetCameraPose(
            camera,
            batch.originalCameraPosition,
            batch.originalCameraYaw,
            batch.originalCameraPitch);
        renderer.ResetPathTracerRestirDiagnosticState();
    }

    void UpdateP7DiagnosticOrbit(
        Scene& scene,
        SceneRenderer& renderer,
        ScreenSpaceEffects& screenSpaceEffects,
        Camera& camera,
        const int viewportWidth,
        const int viewportHeight,
        P7DiagnosticOrbitState& orbit)
    {
        if (!orbit.active)
        {
            return;
        }

        if (renderer.GetDxrSettings().IsRestirGiSpatialEnabled() != orbit.p7Enabled)
        {
            RestoreP7DiagnosticOrbitStart(camera, orbit);
            orbit.active = false;
            screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(false);
            screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
            orbit.status = "Orbit cancelled because the P7 state changed during capture.";
            return;
        }

        const float deltaSeconds = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
        const auto completeOrbit = [&]() {
            RestoreP7DiagnosticOrbitStart(camera, orbit);
            orbit.active = false;
            screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(false);
            const char* diagnosticPath = orbit.p7Enabled
                ? "diagnostics/pt_restir_gi_p7_on.txt"
                : "diagnostics/pt_restir_gi_p7_off.txt";
            std::string reportStatus;
            std::string captureDescription;
            if (orbit.fixedFrameStep)
            {
                const float degreesPerFrame = orbit.targetFrames > 0u
                    ? 360.0f * static_cast<float>(orbit.revolutions)
                        / static_cast<float>(orbit.targetFrames)
                    : 0.0f;
                const float effectiveFps = orbit.measurementWallSeconds > 0.0f
                    ? static_cast<float>(orbit.targetFrames) / orbit.measurementWallSeconds
                    : 0.0f;
                char fixedCapture[512]{};
                std::snprintf(
                    fixedCapture,
                    sizeof(fixedCapture),
                    "Deterministic selected-object orbit: %d revolutions across %u rendered "
                    "diagnostic frames (%.6f degrees/frame); %.3f seconds measurement wall time "
                    "(%.3f effective FPS); final motion metric readback %u/%u frames; XZ-only at "
                    "constant radius/Y; starting pitch preserved; camera returned to start.",
                    orbit.revolutions,
                    orbit.targetFrames,
                    degreesPerFrame,
                    orbit.measurementWallSeconds,
                    effectiveFps,
                    screenSpaceEffects.GetPathTracerGiMotionSampleCount(),
                    orbit.targetFrames);
                captureDescription = fixedCapture;
                if (g_p7DiagnosticBatch.active)
                {
                    const float staticWallSeconds = orbit.p7Enabled
                        ? g_p7DiagnosticBatch.p7OnStaticWallSeconds
                        : g_p7DiagnosticBatch.p7OffStaticWallSeconds;
                    const float staticFps = staticWallSeconds > 0.0f
                        ? static_cast<float>(kP7DiagnosticBatchStaticFrames) / staticWallSeconds
                        : 0.0f;
                    char batchCapture[256]{};
                    std::snprintf(
                        batchCapture,
                        sizeof(batchCapture),
                        " Full automatic P7 A/B sequence: %u-frame history warm-up and %u-frame "
                        "static measurement (%.3f seconds, %.3f effective FPS) before this orbit; "
                        "final static metric readback %u/%u frames; GPU metric readbacks drained "
                        "before report.",
                        kP7DiagnosticBatchWarmupFrames,
                        kP7DiagnosticBatchStaticFrames,
                        staticWallSeconds,
                        staticFps,
                        screenSpaceEffects.GetPathTracerGiStaticSampleCount(),
                        kP7DiagnosticBatchStaticFrames);
                    captureDescription += batchCapture;
                }
            }
            else
            {
                captureDescription = "Realtime selected-object orbit: "
                    + std::to_string(orbit.revolutions)
                    + " revolutions over "
                    + std::to_string(static_cast<int>(orbit.durationSeconds))
                    + " seconds; frame step varies with rendered FPS; XZ-only at constant radius/Y; "
                    "starting pitch preserved; camera returned to start.";
            }
            RenderDiagnostics::WriteReport(
                scene,
                camera,
                viewportWidth,
                viewportHeight,
                diagnosticPath,
                reportStatus,
                captureDescription.c_str());
            orbit.status = std::string(orbit.fixedFrameStep ? "Deterministic " : "Realtime ")
                + std::to_string(orbit.revolutions)
                + "-revolution orbit complete; camera returned to its starting pose. "
                + reportStatus;
        };

        if (orbit.fixedFrameStep)
        {
            if (orbit.preFinalReadbackDrain)
            {
                if (screenSpaceEffects.HasPendingPathTracerTemporalDiagnosticReadbacks())
                {
                    return;
                }
                orbit.preFinalReadbackDrain = false;
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(false);
                orbit.status = "Readbacks drained; recording the final deterministic orbit frame...";
                return;
            }
            if (orbit.finalReadbackDrain)
            {
                if (screenSpaceEffects.HasPendingPathTracerTemporalDiagnosticReadbacks())
                {
                    return;
                }
                completeOrbit();
                return;
            }

            orbit.measurementWallSeconds += deltaSeconds;
            orbit.completedFrames = std::min(
                screenSpaceEffects.GetPathTracerTemporalStatsSampleCount(),
                orbit.targetFrames);
            orbit.elapsedSeconds = orbit.targetFrames > 0u
                ? orbit.durationSeconds * static_cast<float>(orbit.completedFrames)
                    / static_cast<float>(orbit.targetFrames)
                : orbit.durationSeconds;
            if (orbit.completedFrames >= orbit.targetFrames)
            {
                RestoreP7DiagnosticOrbitStart(camera, orbit);
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(true);
                orbit.finalReadbackDrain = true;
                orbit.status = "Orbit frames complete; draining final GPU metric readback...";
                return;
            }

            const float progress = orbit.targetFrames > 0u
                ? static_cast<float>(orbit.completedFrames) / static_cast<float>(orbit.targetFrames)
                : 1.0f;
            const float angle = orbit.startAngle
                + kTwoPi * static_cast<float>(orbit.revolutions) * progress;
            const glm::vec3 position = orbit.target + glm::vec3(
                std::cos(angle) * orbit.horizontalRadius,
                orbit.startPosition.y - orbit.target.y,
                std::sin(angle) * orbit.horizontalRadius);
            camera.SetPosition(position);
            PointCameraYawAtTarget(camera, orbit.target);

            if (orbit.completedFrames + 1u >= orbit.targetFrames)
            {
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(true);
                orbit.preFinalReadbackDrain = true;
                orbit.status = "Preparing the final orbit frame; draining GPU metric readbacks...";
            }
            return;
        }

        // The manual orbit intentionally remains a wall-clock stress test. It reproduces the
        // shipping interaction between estimator cost, rendered FPS, and motion per history update.
        orbit.elapsedSeconds = std::min(
            orbit.elapsedSeconds + deltaSeconds,
            orbit.durationSeconds);
        const float progress = orbit.durationSeconds > 0.0f
            ? orbit.elapsedSeconds / orbit.durationSeconds
            : 1.0f;
        const float angle = orbit.startAngle
            + kTwoPi * static_cast<float>(orbit.revolutions) * progress;
        const glm::vec3 position = orbit.target + glm::vec3(
            std::cos(angle) * orbit.horizontalRadius,
            orbit.startPosition.y - orbit.target.y,
            std::sin(angle) * orbit.horizontalRadius);
        camera.SetPosition(position);
        PointCameraYawAtTarget(camera, orbit.target);
        if (progress >= 1.0f)
        {
            completeOrbit();
        }
    }

    void UpdateP7DiagnosticBatch(
        SceneRenderer& renderer,
        Camera& camera,
        P7DiagnosticOrbitState& orbit,
        P7DiagnosticBatchState& batch)
    {
        if (!batch.active)
        {
            return;
        }

        ScreenSpaceEffects& screenSpaceEffects = renderer.GetScreenSpaceEffects();
        const float deltaSeconds = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
        if (!screenSpaceEffects.ArePathTracerTemporalDiagnosticsPaused()
            && (batch.stage == P7DiagnosticBatchStage::P7OnWarmup
                || batch.stage == P7DiagnosticBatchStage::P7OffWarmup
                || batch.stage == P7DiagnosticBatchStage::P7OnStatic
                || batch.stage == P7DiagnosticBatchStage::P7OffStatic))
        {
            batch.stageMeasurementWallSeconds += deltaSeconds;
        }

        switch (batch.stage)
        {
        case P7DiagnosticBatchStage::P7OnWarmup:
        case P7DiagnosticBatchStage::P7OffWarmup:
        {
            SetCameraPose(
                camera,
                batch.captureCameraPosition,
                batch.captureCameraYaw,
                batch.captureCameraPitch);
            if (batch.stageFinalReadbackDrain)
            {
                if (screenSpaceEffects.HasPendingPathTracerTemporalDiagnosticReadbacks())
                {
                    break;
                }
                batch.stageFinalReadbackDrain = false;
                screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                batch.stage = batch.stage == P7DiagnosticBatchStage::P7OnWarmup
                    ? P7DiagnosticBatchStage::P7OnStatic
                    : P7DiagnosticBatchStage::P7OffStatic;
                batch.stageMeasurementWallSeconds = 0.0f;
                batch.status = batch.stage == P7DiagnosticBatchStage::P7OnStatic
                    ? "A/B capture: measuring 600 P7 ON static frames..."
                    : "A/B capture: measuring 600 P7 OFF static frames...";
                break;
            }
            if (screenSpaceEffects.GetPathTracerTemporalStatsSampleCount()
                >= kP7DiagnosticBatchWarmupFrames)
            {
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(true);
                batch.stageFinalReadbackDrain = true;
                batch.status = "Warm-up complete; draining GPU metric readbacks before measurement...";
            }
            break;
        }
        case P7DiagnosticBatchStage::P7OnStatic:
        case P7DiagnosticBatchStage::P7OffStatic:
        {
            SetCameraPose(
                camera,
                batch.captureCameraPosition,
                batch.captureCameraYaw,
                batch.captureCameraPitch);
            if (batch.stagePreFinalReadbackDrain)
            {
                if (screenSpaceEffects.HasPendingPathTracerTemporalDiagnosticReadbacks())
                {
                    break;
                }
                batch.stagePreFinalReadbackDrain = false;
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(false);
                batch.status = "Readbacks drained; recording the final static frame...";
                break;
            }
            if (batch.stageFinalReadbackDrain)
            {
                if (screenSpaceEffects.HasPendingPathTracerTemporalDiagnosticReadbacks())
                {
                    break;
                }
                batch.stageFinalReadbackDrain = false;
                const bool p7Enabled = batch.stage == P7DiagnosticBatchStage::P7OnStatic;
                if (p7Enabled)
                {
                    batch.p7OnStaticWallSeconds = batch.stageMeasurementWallSeconds;
                }
                else
                {
                    batch.p7OffStaticWallSeconds = batch.stageMeasurementWallSeconds;
                }
                if (!BeginP7DiagnosticOrbit(
                        renderer,
                        camera,
                        batch.target,
                        batch.revolutions,
                        kP7DiagnosticBatchOrbitFrames,
                        orbit,
                        batch.status))
                {
                    RestoreP7DiagnosticBatch(renderer, camera, batch);
                    batch.active = false;
                    batch.stage = P7DiagnosticBatchStage::Idle;
                    break;
                }
                batch.stage = p7Enabled
                    ? P7DiagnosticBatchStage::P7OnOrbit
                    : P7DiagnosticBatchStage::P7OffOrbit;
                batch.stageMeasurementWallSeconds = 0.0f;
                break;
            }

            const std::uint32_t staticFrames =
                screenSpaceEffects.GetPathTracerTemporalStatsSampleCount();
            if (staticFrames >= kP7DiagnosticBatchStaticFrames)
            {
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(true);
                batch.stageFinalReadbackDrain = true;
                batch.status = "Static frames complete; draining final GPU metric readback...";
                break;
            }
            if (staticFrames + 1u >= kP7DiagnosticBatchStaticFrames)
            {
                screenSpaceEffects.SetPathTracerTemporalDiagnosticsPaused(true);
                batch.stagePreFinalReadbackDrain = true;
                batch.status = "Preparing the final static frame; draining GPU metric readbacks...";
            }
            break;
        }
        case P7DiagnosticBatchStage::P7OnOrbit:
            if (!orbit.active)
            {
                ConfigureP7DiagnosticStaticStage(renderer, camera, batch, false);
                batch.stage = P7DiagnosticBatchStage::P7OffWarmup;
                batch.status = "A/B capture: warming P7 OFF history for 60 rendered frames...";
            }
            break;
        case P7DiagnosticBatchStage::P7OffOrbit:
            if (!orbit.active)
            {
                RestoreP7DiagnosticBatch(renderer, camera, batch);
                batch.active = false;
                batch.stage = P7DiagnosticBatchStage::Idle;
                batch.status = "Automatic P7 A/B capture complete. Both reports were written; "
                    "camera and ReSTIR settings restored.";
            }
            break;
        case P7DiagnosticBatchStage::Idle:
        default:
            break;
        }
    }

    std::uint32_t GetP7DiagnosticBatchCompletedFrames(
        const ScreenSpaceEffects& screenSpaceEffects,
        const P7DiagnosticOrbitState& orbit,
        const P7DiagnosticBatchState& batch)
    {
        const std::uint32_t statsFrames = screenSpaceEffects.GetPathTracerTemporalStatsSampleCount();
        const std::uint32_t onWarmupBase = 0u;
        const std::uint32_t onStaticBase = onWarmupBase + kP7DiagnosticBatchWarmupFrames;
        const std::uint32_t onOrbitBase = onStaticBase + kP7DiagnosticBatchStaticFrames;
        const std::uint32_t offWarmupBase = onOrbitBase + kP7DiagnosticBatchOrbitFrames;
        const std::uint32_t offStaticBase = offWarmupBase + kP7DiagnosticBatchWarmupFrames;
        const std::uint32_t offOrbitBase = offStaticBase + kP7DiagnosticBatchStaticFrames;
        switch (batch.stage)
        {
        case P7DiagnosticBatchStage::P7OnWarmup:
            return onWarmupBase + std::min(statsFrames, kP7DiagnosticBatchWarmupFrames);
        case P7DiagnosticBatchStage::P7OnStatic:
            return onStaticBase + std::min(statsFrames, kP7DiagnosticBatchStaticFrames);
        case P7DiagnosticBatchStage::P7OnOrbit:
            return onOrbitBase + std::min(orbit.completedFrames, kP7DiagnosticBatchOrbitFrames);
        case P7DiagnosticBatchStage::P7OffWarmup:
            return offWarmupBase + std::min(statsFrames, kP7DiagnosticBatchWarmupFrames);
        case P7DiagnosticBatchStage::P7OffStatic:
            return offStaticBase + std::min(statsFrames, kP7DiagnosticBatchStaticFrames);
        case P7DiagnosticBatchStage::P7OffOrbit:
            return offOrbitBase + std::min(orbit.completedFrames, kP7DiagnosticBatchOrbitFrames);
        case P7DiagnosticBatchStage::Idle:
        default:
            return 0u;
        }
    }

    bool ComputeSelectedObjectScreenRoi(
        const Scene& scene,
        const Camera& camera,
        const bool contactBand,
        glm::vec4& outRoi)
    {
        const int selectedIndex = scene.GetPrimarySelection();
        if (selectedIndex < 0)
        {
            return false;
        }

        glm::vec3 boundsMin;
        glm::vec3 boundsMax;
        scene.GetWorldBounds(selectedIndex, boundsMin, boundsMax);
        const std::array<glm::vec3, 8> corners = {
            glm::vec3(boundsMin.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMin.z),
            glm::vec3(boundsMin.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMin.y, boundsMax.z),
            glm::vec3(boundsMin.x, boundsMax.y, boundsMax.z),
            glm::vec3(boundsMax.x, boundsMax.y, boundsMax.z),
        };

        const glm::mat4 viewProjection =
            camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix();
        glm::vec2 uvMin(std::numeric_limits<float>::max());
        glm::vec2 uvMax(std::numeric_limits<float>::lowest());
        bool projectedAny = false;
        for (const glm::vec3& corner : corners)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
            if (clip.w <= 1.0e-4f)
            {
                continue;
            }
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const glm::vec2 uv(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            uvMin = glm::min(uvMin, uv);
            uvMax = glm::max(uvMax, uv);
            projectedAny = true;
        }
        if (!projectedAny)
        {
            return false;
        }

        const glm::vec2 extent = glm::max(uvMax - uvMin, glm::vec2(0.02f));
        if (contactBand)
        {
            uvMin.y = uvMax.y - extent.y * 0.35f;
            uvMax.y += extent.y * 0.12f;
        }
        const glm::vec2 padding = glm::max(extent * 0.06f, glm::vec2(0.005f));
        uvMin -= padding;
        uvMax += padding;
        uvMin = glm::clamp(uvMin, glm::vec2(0.0f), glm::vec2(0.99f));
        uvMax = glm::clamp(uvMax, uvMin + glm::vec2(0.01f), glm::vec2(1.0f));
        outRoi = glm::vec4(uvMin, uvMax);
        return true;
    }
}

void UpdateRayTracingDiagnosticCapture(
    Scene& scene,
    Camera& camera,
    const int viewportWidth,
    const int viewportHeight)
{
    SceneRenderer& renderer = scene.GetRenderer();
    if (!renderer.IsGpuResourcesReady())
    {
        return;
    }
    ScreenSpaceEffects& screenSpaceEffects = renderer.GetScreenSpaceEffects();
    UpdateP7DiagnosticOrbit(
        scene,
        renderer,
        screenSpaceEffects,
        camera,
        viewportWidth,
        viewportHeight,
        g_p7DiagnosticOrbit);
    UpdateP7DiagnosticBatch(renderer, camera, g_p7DiagnosticOrbit, g_p7DiagnosticBatch);
}

void DrawRayTracingSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    Camera& camera = ctx.camera;
    P7DiagnosticOrbitState& diagnosticOrbit = g_p7DiagnosticOrbit;
    P7DiagnosticBatchState& diagnosticBatch = g_p7DiagnosticBatch;

    if (TuningSectionState::SectionHeader("Ray tracing", true))
    {
        const GfxContext& gfx = GfxContext::Get();
        const bool raytracingSupported = gfx.IsInitialized() && gfx.IsRaytracingSupported();
        const int raytracingTier = gfx.IsInitialized() ? gfx.GetRaytracingTier() : 0;
        const int shaderModel = gfx.IsInitialized() ? gfx.GetHighestShaderModel() : 0;
        const std::string& adapterName =
            gfx.IsInitialized() ? gfx.GetAdapterDescription() : std::string("(GPU not initialized)");

        ImGui::Text("Adapter: %s", adapterName.c_str());
        char tierText[64]{};
        FormatRaytracingTierText(raytracingTier, tierText, sizeof(tierText));
        ImGui::Text("Ray tracing tier: %s", tierText);
        ImGui::Text("Shader model: %s", GetShaderModelLabel(shaderModel));
        ImGui::Text(
            "Inline ray tracing: %s",
            gfx.IsInlineRaytracingSupported()
                ? "enabled for PT opaque visibility"
                : "unavailable (legacy visibility fallback)");
        ImGui::Text(
            "Shader execution reordering: %s",
            gfx.IsShaderExecutionReorderingSupported()
                ? (renderer.IsPathTracerSerActive() ? "active for PT path rays" : "available (automatic)")
                : "unavailable");
        ImGui::Text("DXR library profile: %s", gfx.GetPreferredDxrLibraryProfile());

        if (!raytracingSupported)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                "Ray tracing requires a DXR Tier 1.0+ GPU and up-to-date driver.");
            if (raytracingTier == 0 && gfx.IsInitialized())
            {
                LightingPanelUi::DrawWrappedNote(
                    "Update your graphics driver (NVIDIA 531+ class recommended) if you expect RTX support.");
            }
        }

        DxrSettings& dxrSettings = renderer.GetDxrSettings();

        // RR5: when Ray Reconstruction is running it replaces the NRD denoisers entirely, so their
        // tuning controls are inert. Render them disabled with a reason (not hidden) — toggling RR off
        // brings them back live. The RT feature enables + trace params stay live (RR consumes them).
        const bool rrActive = screenSpaceEffects.IsRayReconstructionActive();

        if (!raytracingSupported)
        {
            ImGui::BeginDisabled();
        }

        ImGui::PushID("RayTracing");
        bool dxrEnabled = dxrSettings.IsEnabled();
        const bool pathTracingActive =
            dxrEnabled && dxrSettings.GetRenderingMode() == RenderingMode::PathTraced;
        const bool ptRrSupported = DlssContext::Get().IsReady() && DlssContext::Get().IsRrSupported();
        UndoableRendererCheckbox(
            "Enable ray tracing",
            &dxrEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                EngineLog::Breadcrumb(
                    "dxr",
                    std::string("editor: Enable ray tracing -> ") + (enabled ? "on" : "off"));
                if (enabled)
                {
                    ResetDxrBreadcrumbOnceFlags();
                }
                target.GetRenderer().GetDxrSettings().SetEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.GetRenderer().GetScreenSpaceEffects().InvalidateSsrHistory();
                target.MarkDirty();
            });
        RendererSettingUi::MarkRendered("raytracing_enabled");

        // Phase P0 — rendering mode (devdoc/dxr/path-tracing.md). Hybrid (raster + hybrid RT, the
        // default) vs the unified path tracer. Path tracing needs master RT on; greyed otherwise.
        const bool renderingModeSelectable = dxrEnabled;
        if (!renderingModeSelectable)
        {
            ImGui::BeginDisabled();
        }
        int renderingModeIndex = static_cast<int>(dxrSettings.GetRenderingMode());
        const char* renderingModeLabels[] = {"Hybrid (raster + RT)", "Path traced"};
        if (ImGui::Combo(
                "Rendering mode",
                &renderingModeIndex,
                renderingModeLabels,
                IM_ARRAYSIZE(renderingModeLabels)))
        {
            const auto mode = static_cast<RenderingMode>(renderingModeIndex);
            RendererSettingUi::ApplyChange(
                "path_tracing",
                editContext,
                scene,
                "Rendering mode",
                [mode](Scene& target) {
                    target.GetRenderer().GetDxrSettings().SetRenderingMode(mode);
                    if (mode == RenderingMode::PathTraced && !GfxContext::Get().IsFrameRecording())
                    {
                        target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                    }
                    target.MarkDirty();
                });
        }
        if (ImGui::IsItemHovered() && renderingModeSelectable)
        {
            ImGui::SetTooltip(
                "Path traced: the unified path tracer owns the image (P0 shows primary-hit normals).\n"
                "Hybrid keeps the raster + RT pipeline. Additive - both are selectable for comparison.");
        }
        RendererSettingUi::MarkRendered("path_tracing");
        if (!renderingModeSelectable)
        {
            ImGui::EndDisabled();
            ImGui::TextDisabled("    Enable ray tracing to use path tracing.");
        }

        if (dxrSettings.GetRenderingMode() == RenderingMode::PathTraced && dxrEnabled)
        {
            // PF7 runtime A/B selector. This belongs to DxrPathTracerDispatch, rather than
            // DxrSettings, so it never becomes scene/project state or an undo command.
            int serOverride = static_cast<int>(renderer.GetPathTracerSerOverride());
            const char* serOverrideLabels[] = {"Automatic", "Force off", "Force on"};
            if (ImGui::Combo(
                    "PT SER (debug)",
                    &serOverride,
                    serOverrideLabels,
                    IM_ARRAYSIZE(serOverrideLabels)))
            {
                renderer.SetPathTracerSerOverride(
                    static_cast<DxrPathTracerDispatch::SerOverride>(serOverride));
                // All permutations are warmed before interactive rendering. Reset reference
                // accumulation as well as the dispatcher's ReSTIR history on the switch.
                renderer.WarmUpDxrPipelineIfNeeded();
                screenSpaceEffects.ResetPathTracerAccumulation();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Runtime-only A/B selector. Automatic uses SER when the GPU supports native\n"
                    "HitObject reordering; Force on still falls back safely on unsupported hardware.");
            }

            const bool rrConvergenceSelectable = ptRrSupported;
            if (!rrConvergenceSelectable)
            {
                ImGui::BeginDisabled();
            }
            int convergenceModeIndex = static_cast<int>(dxrSettings.GetPtConvergenceMode());
            const char* convergenceModeLabels[] = {"Real-time (DLSS-RR)", "Reference (accumulate)"};
            if (ImGui::Combo(
                    "PT convergence",
                    &convergenceModeIndex,
                    convergenceModeLabels,
                    IM_ARRAYSIZE(convergenceModeLabels)))
            {
                const auto mode = static_cast<PtConvergenceMode>(convergenceModeIndex);
                if (mode == PtConvergenceMode::RealTime && !ptRrSupported)
                {
                    // Combo can still be changed programmatically; keep Reference when RR is absent.
                }
                else
                {
                    RendererSettingUi::ApplyChange(
                        "pt_convergence",
                        editContext,
                        scene,
                        "PT convergence mode",
                        [mode](Scene& target) {
                            target.GetRenderer().GetDxrSettings().SetPtConvergenceMode(mode);
                            target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                            target.MarkDirty();
                        });
                }
            }
            if (!rrConvergenceSelectable)
            {
                ImGui::EndDisabled();
                ImGui::TextDisabled("    Real-time needs DLSS Ray Reconstruction on this GPU.");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Reference: progressive HDR accumulation while the camera and scene are static.\n"
                    "Resets on camera move, resize, light/scene edits, or setting changes.\n"
                    "Real-time: 1 spp path trace denoised via DLSS Ray Reconstruction.");
            }

            RendererSettingUi::MarkRendered("pt_convergence");

            // Diagnostic switchboard (devdoc/dxr/pt/gi-shimmer.md): which RR inputs come from the
            // PT vs raster. Direct set, no undo — this is a debug control, not scene state.
            int rrBundleMode = dxrSettings.GetPtRrBundleMode();
            const char* rrBundleLabels[] = {
                "Full PT (depth+motion+guides)",
                "Raster bundle (stable fallback)",
                "PT guides only",
                "PT depth+motion",
                "PT depth only",
                "PT motion only"};
            if (ImGui::Combo(
                    "RR inputs (debug)",
                    &rrBundleMode,
                    rrBundleLabels,
                    IM_ARRAYSIZE(rrBundleLabels)))
            {
                dxrSettings.SetPtRrBundleMode(rrBundleMode);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Shimmer isolation: pick which DLSS-RR inputs come from the path tracer vs the\n"
                    "raster G-buffer. 'Raster bundle' is the previous stable configuration.\n"
                    "Full PT uses merged motion (raster geometry + PT sky); modes 3/5 use raw PT\n"
                    "motion for diagnosis only.");
            }

            int ptMaxBounces = dxrSettings.GetPtMaxBounces();
            UndoableRendererSliderInt(
                "PT max bounces",
                &ptMaxBounces,
                1,
                16,
                editContext,
                [](Scene& target, int bounces) {
                    target.GetRenderer().GetDxrSettings().SetPtMaxBounces(bounces);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            RendererSettingUi::MarkRendered("pt_max_bounces");

            bool ptRussianRoulette = dxrSettings.IsPtRussianRouletteEnabled();
            UndoableRendererCheckbox(
                "PT Russian roulette",
                &ptRussianRoulette,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetPtRussianRouletteEnabled(enabled);
                    target.MarkDirty();
                });

            bool ptFireflyClamp = dxrSettings.IsPtFireflyClampEnabled();
            UndoableRendererCheckbox(
                "PT firefly clamp",
                &ptFireflyClamp,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetPtFireflyClampEnabled(enabled);
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Clamps rare ultra-bright path samples before denoise/accumulate.\n"
                    "Slightly biased; turn off in Reference for ground truth.");
            }

            float ptAmbientStrength = dxrSettings.GetPtAmbientStrength();
            UndoableRendererSliderFloat(
                "PT ambient strength",
                &ptAmbientStrength,
                0.0f,
                2.0f,
                "%.2f",
                editContext,
                [](Scene& target, float strength) {
                    target.GetRenderer().GetDxrSettings().SetPtAmbientStrength(strength);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Real-time only: scales the AO-gated SH sky ambient at the primary hit.\n"
                    "Independent of sun intensity. 0 = no ambient floor (crevices go black).");
            }

            int ptAmbientAoRayCount = dxrSettings.GetPtAmbientAoRayCount();
            UndoableRendererSliderInt(
                "PT ambient AO rays",
                &ptAmbientAoRayCount,
                0,
                8,
                editContext,
                [](Scene& target, int rays) {
                    target.GetRenderer().GetDxrSettings().SetPtAmbientAoRayCount(rays);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Real-time only: short cosine visibility rays that darken SH ambient in crevices.\n"
                    "0 = unoccluded ambient (recommended). Raise only if open shadows wash out.");
            }

            int restirDiCandidates = dxrSettings.GetRestirDiCandidateCount();
            UndoableRendererSliderInt(
                "PT ReSTIR DI candidates",
                &restirDiCandidates,
                0,
                16,
                editContext,
                [](Scene& target, int count) {
                    target.GetRenderer().GetDxrSettings().SetRestirDiCandidateCount(count);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "ReSTIR DI initial sampling for bounce-0 emissive + environment direct light\n"
                    "(roadmap P2). 0 = off (plain NEE). 1 = one candidate each — should look IDENTICAL\n"
                    "to off (converged), the A/B parity check. N>1 = resample N candidates with one\n"
                    "shadow ray each: less emissive/env noise at equal cost, same converged image.");
            }

            RendererSettingUi::MarkRendered("pt_restir_di_candidates");

            bool restirDiTemporal = dxrSettings.IsRestirDiTemporalEnabled();
            UndoableRendererCheckbox(
                "PT ReSTIR DI temporal",
                &restirDiTemporal,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetRestirDiTemporalEnabled(enabled);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "P3 temporal reuse for typed emissive/environment reservoirs. Real-time opaque\n"
                    "surfaces only; reference, transmission, delta lobes, and rejected history use fresh DI.");
            }

            RendererSettingUi::MarkRendered("pt_restir_di_temporal");

            if (diagnosticBatch.active)
            {
                ImGui::BeginDisabled();
            }

            bool restirGiInitial = dxrSettings.IsRestirGiInitialEnabled();
            UndoableRendererCheckbox(
                "PT ReSTIR GI initial (P5)",
                &restirGiInitial,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetRestirGiInitialEnabled(enabled);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerTemporalDiagnostics();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "P5 M=1 production GI payload and receiver-side reconstruction. Eligible diffuse/\n"
                    "moderately rough opaque primaries should match baseline PT. Glass, delta, and\n"
                    "smooth primaries remain on the original estimator. P6/P7 reuse require this input.");
            }
            RendererSettingUi::MarkRendered("pt_restir_gi_initial");

            bool restirGiTemporal = dxrSettings.IsRestirGiTemporalEnabled();
            UndoableRendererCheckbox(
                "PT ReSTIR GI temporal (P6)",
                &restirGiTemporal,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetRestirGiTemporalEnabled(enabled);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerTemporalDiagnostics();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "P6 temporal reuse of P5 secondary-surface GI reservoirs. Requires P5 initial.\n"
                    "Uses receiver reevaluation, reconnection Jacobian, BASIC bias correction, and\n"
                    "a current-receiver visibility ray. Every rejection falls back to the fresh sample.");
            }
            RendererSettingUi::MarkRendered("pt_restir_gi_temporal");

            bool restirGiSpatial = dxrSettings.IsRestirGiSpatialEnabled();
            UndoableRendererCheckbox(
                "PT ReSTIR GI spatial (P7)",
                &restirGiSpatial,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetDxrSettings().SetRestirGiSpatialEnabled(enabled);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerTemporalDiagnostics();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "P7 spatial reuse of P5/P6 secondary-surface GI reservoirs. Requires P5 initial.\n"
                    "Uses material/normal/depth gates, reconnection Jacobians, BASIC multi-domain\n"
                    "normalization, conservative visibility, boiling control, and input/output MIS.");
            }
            RendererSettingUi::MarkRendered("pt_restir_gi_spatial");

            if (diagnosticBatch.active)
            {
                ImGui::EndDisabled();
            }

            float ptSunAngularRadius = dxrSettings.GetSunAngularRadiusDegrees();
            UndoableRendererSliderFloat(
                "PT sun angular radius",
                &ptSunAngularRadius,
                0.05f,
                2.0f,
                "%.2f deg",
                editContext,
                [](Scene& target, float degrees) {
                    target.GetRenderer().GetDxrSettings().SetSunAngularRadiusDegrees(degrees);
                    target.GetRenderer().GetScreenSpaceEffects().ResetPathTracerAccumulation();
                    target.MarkDirty();
                });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Angular radius of the sun disk for soft shadow penumbra (real-time PT NEE).\n"
                    "~0.27 deg matches the real sun. Wider = softer contact shadows. Shared with RT shadows.");
            }

            if (dxrSettings.IsPtReferenceConvergence())
            {
                const std::uint32_t spp =
                    scene.GetRenderer().GetScreenSpaceEffects().GetPathTracerAccumSampleCount();
                if (spp > 0u)
                {
                    // Spp alone cannot prove convergence; high-dynamic-range paths can retain
                    // measurable variance for thousands of samples. Never label this converged
                    // without an actual confidence/variance criterion.
                    ImGui::TextDisabled("    Reference: %u spp (accumulating...)", spp);
                }
                else
                {
                    ImGui::TextDisabled("    Reference: accumulating...");
                }
            }
            else
            {
                if (screenSpaceEffects.PathTracerResolvedViaDlssThisFrame())
                {
                    ImGui::TextDisabled("    Real-time: DLSS-RR reconstructed this frame");
                }
                else if (ptRrSupported)
                {
                    ImGui::TextDisabled("    Real-time: awaiting DLSS-RR...");
                }
                const float pathTracerMs = FindGpuPassMilliseconds("Path tracer");
                if (pathTracerMs >= 0.0f)
                {
                    ImGui::TextDisabled("    Path tracer GPU: %.3f ms", pathTracerMs);
                }
                const std::uint32_t statsSamples =
                    screenSpaceEffects.GetPathTracerTemporalStatsSampleCount();
                if (screenSpaceEffects.IsPathTracerBoilMetricValid())
                {
                    ImGui::TextDisabled(
                        "    PT frame-instability metric: %.5f (%u samples)",
                        screenSpaceEffects.GetPathTracerBoilMetric(),
                        statsSamples);
                }
                else if (statsSamples > 0u)
                {
                    ImGui::TextDisabled(
                        "    PT frame-instability metric: pending (%u samples)",
                        statsSamples);
                }

                {
                    ImGui::Indent();
                    ImGui::TextDisabled("P6/P7 GI diagnostics (screen-space ROI)");
                    static std::string roiStatus;
                    static int requestedOrbitRevolutions = 3;
                    glm::vec3 selectedTarget;
                    float selectedRadius = 0.5f;
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderInt(
                        "Orbit revolutions##pt-restir-gi-camera",
                        &requestedOrbitRevolutions,
                        1,
                        20);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "Automatic A/B: this many revolutions across exactly 600 rendered frames.\n"
                            "Manual realtime orbit: this many revolutions over 10 wall-clock seconds.");
                    }

                    const bool batchWasActive = diagnosticBatch.active;
                    if (!batchWasActive
                        && ImGui::Button("Run complete P7 ON/OFF capture##pt-restir-gi-batch"))
                    {
                        if (!ComputePrimarySelectionFocus(scene, selectedTarget, selectedRadius))
                        {
                            diagnosticBatch.status = "Select a renderable object first.";
                        }
                        else
                        {
                            const glm::vec3 offset = camera.GetPosition() - selectedTarget;
                            if (glm::length(glm::vec2(offset.x, offset.z))
                                < std::max(0.1f, selectedRadius * 0.1f))
                            {
                                diagnosticBatch.status =
                                    "Camera is too close to directly above/below the selection. Frame it first.";
                            }
                            else
                            {
                                diagnosticBatch = {};
                                diagnosticBatch.active = true;
                                diagnosticBatch.stage = P7DiagnosticBatchStage::P7OnWarmup;
                                diagnosticBatch.revolutions = requestedOrbitRevolutions;
                                diagnosticBatch.originalP5Enabled =
                                    dxrSettings.IsRestirGiInitialEnabled();
                                diagnosticBatch.originalP6Enabled =
                                    dxrSettings.IsRestirGiTemporalEnabled();
                                diagnosticBatch.originalP7Enabled =
                                    dxrSettings.IsRestirGiSpatialEnabled();
                                diagnosticBatch.originalDebugMode = renderer.GetRenderDebugMode();
                                diagnosticBatch.originalCameraPosition = camera.GetPosition();
                                diagnosticBatch.originalCameraYaw = camera.GetYaw();
                                diagnosticBatch.originalCameraPitch = camera.GetPitch();
                                diagnosticBatch.target = selectedTarget;
                                PointCameraYawAtTarget(camera, selectedTarget);
                                diagnosticBatch.captureCameraPosition = camera.GetPosition();
                                diagnosticBatch.captureCameraYaw = camera.GetYaw();
                                diagnosticBatch.captureCameraPitch = camera.GetPitch();
                                ConfigureP7DiagnosticStaticStage(
                                    renderer,
                                    camera,
                                    diagnosticBatch,
                                    true);
                                diagnosticBatch.status =
                                    "A/B capture: warming P7 ON history for 60 rendered frames...";
                            }
                        }
                    }
                    if (diagnosticBatch.active)
                    {
                        constexpr std::uint32_t totalCaptureFrames = 2u * (
                            kP7DiagnosticBatchWarmupFrames
                            + kP7DiagnosticBatchStaticFrames
                            + kP7DiagnosticBatchOrbitFrames);
                        const std::uint32_t completedCaptureFrames =
                            GetP7DiagnosticBatchCompletedFrames(
                                screenSpaceEffects,
                                diagnosticOrbit,
                                diagnosticBatch);
                        const float batchProgress = static_cast<float>(completedCaptureFrames)
                            / static_cast<float>(totalCaptureFrames);
                        char batchProgressLabel[64]{};
                        std::snprintf(
                            batchProgressLabel,
                            sizeof(batchProgressLabel),
                            "%u / %u rendered frames",
                            completedCaptureFrames,
                            totalCaptureFrames);
                        ImGui::ProgressBar(
                            batchProgress,
                            ImVec2(-1.0f, 0.0f),
                            batchProgressLabel);
                        if (ImGui::Button("Cancel complete capture##pt-restir-gi-batch"))
                        {
                            if (diagnosticOrbit.active)
                            {
                                RestoreP7DiagnosticOrbitStart(camera, diagnosticOrbit);
                                diagnosticOrbit.active = false;
                            }
                            RestoreP7DiagnosticBatch(renderer, camera, diagnosticBatch);
                            diagnosticBatch.active = false;
                            diagnosticBatch.stage = P7DiagnosticBatchStage::Idle;
                            diagnosticBatch.status =
                                "Automatic P7 A/B capture cancelled; camera and settings restored.";
                        }
                    }
                    if (!diagnosticBatch.status.empty())
                    {
                        LightingPanelUi::DrawWrappedHelp(diagnosticBatch.status.c_str());
                    }

                    const bool orbitWasActive = diagnosticOrbit.active;
                    if (orbitWasActive || batchWasActive)
                    {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Button("Point at selected##pt-restir-gi-camera"))
                    {
                        if (ComputePrimarySelectionFocus(scene, selectedTarget, selectedRadius))
                        {
                            if (PointCameraYawAtTarget(camera, selectedTarget))
                            {
                                screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                                diagnosticOrbit.status =
                                    "Camera yaw now points at the selection; pitch and position preserved.";
                            }
                            else
                            {
                                diagnosticOrbit.status =
                                    "Camera is directly above/below the selection; horizontal aim is undefined.";
                            }
                        }
                        else
                        {
                            diagnosticOrbit.status = "Select a renderable object first.";
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Frame selected##pt-restir-gi-camera"))
                    {
                        if (ComputePrimarySelectionFocus(scene, selectedTarget, selectedRadius))
                        {
                            camera.FrameTarget(selectedTarget, selectedRadius);
                            screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                            diagnosticOrbit.status =
                                "Selected object framed. Adjust the camera/ROI now if you want a tighter contact capture.";
                        }
                        else
                        {
                            diagnosticOrbit.status = "Select a renderable object first.";
                        }
                    }
                    if (ImGui::Button("Start repeatable orbit##pt-restir-gi-camera"))
                    {
                        if (!ComputePrimarySelectionFocus(scene, selectedTarget, selectedRadius))
                        {
                            diagnosticOrbit.status = "Select a renderable object first.";
                        }
                        else
                        {
                            const glm::vec3 startPosition = camera.GetPosition();
                            const glm::vec3 offset = startPosition - selectedTarget;
                            const float horizontalRadius = glm::length(glm::vec2(offset.x, offset.z));
                            if (horizontalRadius < std::max(0.1f, selectedRadius * 0.1f))
                            {
                                diagnosticOrbit.status =
                                    "Camera is too close to directly above/below the selection. Frame it first.";
                            }
                            else
                            {
                                diagnosticOrbit.active = true;
                                diagnosticOrbit.p7Enabled = dxrSettings.IsRestirGiSpatialEnabled();
                                diagnosticOrbit.elapsedSeconds = 0.0f;
                                diagnosticOrbit.durationSeconds = kP7DiagnosticOrbitDurationSeconds;
                                diagnosticOrbit.revolutions = requestedOrbitRevolutions;
                                diagnosticOrbit.horizontalRadius = horizontalRadius;
                                diagnosticOrbit.startAngle = std::atan2(offset.z, offset.x);
                                diagnosticOrbit.target = selectedTarget;
                                diagnosticOrbit.startPosition = startPosition;
                                diagnosticOrbit.fixedFrameStep = false;
                                diagnosticOrbit.preFinalReadbackDrain = false;
                                diagnosticOrbit.finalReadbackDrain = false;
                                diagnosticOrbit.targetFrames = 0u;
                                diagnosticOrbit.completedFrames = 0u;
                                diagnosticOrbit.measurementWallSeconds = 0.0f;
                                PointCameraYawAtTarget(camera, selectedTarget);
                                diagnosticOrbit.startYaw = camera.GetYaw();
                                diagnosticOrbit.startPitch = camera.GetPitch();
                                renderer.SetRenderDebugMode(
                                    RenderDebugMode::PtRestirGiSpatialMotionDelta);
                                screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                                diagnosticOrbit.status = std::string("Recording repeatable P7 ")
                                    + (diagnosticOrbit.p7Enabled ? "ON" : "OFF")
                                    + " orbit ("
                                    + std::to_string(diagnosticOrbit.revolutions)
                                    + " revolutions / 10 s)...";
                            }
                        }
                    }
                    if (orbitWasActive || batchWasActive)
                    {
                        ImGui::EndDisabled();
                    }
                    if (diagnosticOrbit.active && !diagnosticBatch.active)
                    {
                        const float progress = diagnosticOrbit.durationSeconds > 0.0f
                            ? diagnosticOrbit.elapsedSeconds / diagnosticOrbit.durationSeconds
                            : 1.0f;
                        char progressLabel[48]{};
                        std::snprintf(
                            progressLabel,
                            sizeof(progressLabel),
                            "%.1f / %.1f s",
                            diagnosticOrbit.elapsedSeconds,
                            diagnosticOrbit.durationSeconds);
                        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), progressLabel);
                        if (ImGui::Button("Cancel orbit##pt-restir-gi-camera"))
                        {
                            RestoreP7DiagnosticOrbitStart(camera, diagnosticOrbit);
                            diagnosticOrbit.active = false;
                            screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                            diagnosticOrbit.status =
                                "Orbit cancelled; camera returned to its starting pose.";
                        }
                    }
                    if (!diagnosticOrbit.status.empty())
                    {
                        LightingPanelUi::DrawWrappedHelp(diagnosticOrbit.status.c_str());
                    }
                    glm::vec4 selectedRoi;
                    if (ImGui::Button("ROI: selected object##pt-restir-gi"))
                    {
                        if (ComputeSelectedObjectScreenRoi(scene, ctx.camera, false, selectedRoi))
                        {
                            screenSpaceEffects.SetPathTracerGiDiagnosticRoi(selectedRoi);
                            scene.MarkDirty();
                            roiStatus = "ROI fitted to the selected object's projected bounds.";
                        }
                        else
                        {
                            roiStatus = "Select a visible renderable object first.";
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("ROI: selected contact##pt-restir-gi"))
                    {
                        if (ComputeSelectedObjectScreenRoi(scene, ctx.camera, true, selectedRoi))
                        {
                            screenSpaceEffects.SetPathTracerGiDiagnosticRoi(selectedRoi);
                            scene.MarkDirty();
                            roiStatus = "ROI fitted to the bottom/contact band of the selected object.";
                        }
                        else
                        {
                            roiStatus = "Select a visible renderable object first.";
                        }
                    }
                    if (ImGui::SmallButton("Full frame##pt-restir-gi-roi"))
                    {
                        screenSpaceEffects.SetPathTracerGiDiagnosticRoi(
                            glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
                        scene.MarkDirty();
                        roiStatus = "ROI set to the full frame.";
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset measurement##pt-restir-gi"))
                    {
                        screenSpaceEffects.ResetPathTracerTemporalDiagnostics();
                    }
                    const glm::vec4 roi = screenSpaceEffects.GetPathTracerGiDiagnosticRoi();
                    ImGui::TextDisabled(
                        "ROI pixels: (%d, %d) to (%d, %d)",
                        static_cast<int>(roi.x * static_cast<float>(ctx.viewportWidth)),
                        static_cast<int>(roi.y * static_cast<float>(ctx.viewportHeight)),
                        static_cast<int>(roi.z * static_cast<float>(ctx.viewportWidth)),
                        static_cast<int>(roi.w * static_cast<float>(ctx.viewportHeight)));
                    if (!roiStatus.empty())
                    {
                        ImGui::TextDisabled("%s", roiStatus.c_str());
                    }
                    if (ImGui::TreeNode("Advanced ROI coordinates##pt-restir-gi"))
                    {
                        glm::vec2 roiCenter = 0.5f * (glm::vec2(roi) + glm::vec2(roi.z, roi.w));
                        glm::vec2 roiHalfExtent =
                            0.5f * (glm::vec2(roi.z, roi.w) - glm::vec2(roi));
                        bool roiChanged = ImGui::DragFloat2(
                            "Center##pt-restir-gi",
                            glm::value_ptr(roiCenter),
                            0.005f,
                            0.0f,
                            1.0f,
                            "%.3f");
                        roiChanged |= ImGui::DragFloat2(
                            "Half extent##pt-restir-gi",
                            glm::value_ptr(roiHalfExtent),
                            0.005f,
                            0.01f,
                            0.5f,
                            "%.3f");
                        if (roiChanged)
                        {
                            roiHalfExtent = glm::clamp(
                                roiHalfExtent,
                                glm::vec2(0.01f),
                                glm::vec2(0.5f));
                            roiCenter = glm::clamp(
                                roiCenter,
                                roiHalfExtent,
                                glm::vec2(1.0f) - roiHalfExtent);
                            screenSpaceEffects.SetPathTracerGiDiagnosticRoi(glm::vec4(
                                roiCenter - roiHalfExtent,
                                roiCenter + roiHalfExtent));
                            scene.MarkDirty();
                        }
                        ImGui::TreePop();
                    }
                    if (screenSpaceEffects.IsPathTracerGiStaticMetricValid())
                    {
                        ImGui::TextDisabled(
                            "Static GI: delta %.5f (rel %.4f), sigma/mean %.4f, mean %.4f (%u frames)",
                            screenSpaceEffects.GetPathTracerGiStaticDelta(),
                            screenSpaceEffects.GetPathTracerGiStaticRelativeDelta(),
                            screenSpaceEffects.GetPathTracerGiStaticRelativeSigma(),
                            screenSpaceEffects.GetPathTracerGiStaticMeanLuminance(),
                            screenSpaceEffects.GetPathTracerGiStaticSampleCount());
                    }
                    else
                    {
                        ImGui::TextDisabled("Static GI: select 'static variance' debug view");
                    }
                    if (screenSpaceEffects.IsPathTracerGiMotionMetricValid())
                    {
                        ImGui::TextDisabled(
                            "Motion GI: abs %.5f, relative %.4f, valid %.1f%% (%u frames)",
                            screenSpaceEffects.GetPathTracerGiMotionDelta(),
                            screenSpaceEffects.GetPathTracerGiMotionRelativeDelta(),
                            100.0f * screenSpaceEffects.GetPathTracerGiMotionValidFraction(),
                            screenSpaceEffects.GetPathTracerGiMotionSampleCount());
                        ImGui::TextDisabled(
                            "    tail rel: p95 %.3f, p99 %.3f, session peak %.3f, hot %.1f%%",
                            screenSpaceEffects.GetPathTracerGiMotionP95RelativeDelta(),
                            screenSpaceEffects.GetPathTracerGiMotionP99RelativeDelta(),
                            screenSpaceEffects.GetPathTracerGiMotionPeakRelativeDelta(),
                            100.0f * screenSpaceEffects.GetPathTracerGiMotionHotFraction());
                        ImGui::TextDisabled(
                            "    coherence: neighbor %.3f, low-frequency %.3f, blurred-hot %.1f%%",
                            screenSpaceEffects.GetPathTracerGiMotionNeighborCorrelation(),
                            screenSpaceEffects.GetPathTracerGiMotionLowFrequencyRatio(),
                            100.0f * screenSpaceEffects.GetPathTracerGiMotionBlurredHotFraction());
                        ImGui::TextDisabled(
                            "    ROI halves p99/hot: upper %.3f / %.1f%%, lower %.3f / %.1f%%",
                            screenSpaceEffects.GetPathTracerGiMotionUpperP99RelativeDelta(),
                            100.0f * screenSpaceEffects.GetPathTracerGiMotionUpperHotFraction(),
                            screenSpaceEffects.GetPathTracerGiMotionLowerP99RelativeDelta(),
                            100.0f * screenSpaceEffects.GetPathTracerGiMotionLowerHotFraction());
                    }
                    else
                    {
                        ImGui::TextDisabled("Motion GI: select 'motion-reprojected delta' debug view");
                    }
                    LightingPanelUi::DrawWrappedHelp(
                        "Complete A/B uses matched 60-frame warm-up, 600-frame static, and 600-frame "
                        "orbit captures for P7 ON then OFF. Manual repeatable orbit remains a "
                        "10-second realtime stress test. Both return to the exact starting pose and "
                        "write the matching P7 ON/OFF report automatically.");
                    LightingPanelUi::DrawWrappedHelp(
                        "Tail values expose rare bright changes. Hot means relative delta >= 1. "
                        "Neighbor correlation and low-frequency variance retention rise when fine "
                        "grain organizes into broad moving patches. Upper/lower are screen-space ROI "
                        "halves, not object masks.");
                    LightingPanelUi::DrawWrappedHelp(
                        "The ROI is stored with the project's renderer settings when the project is saved.");
                    static std::string p7DiagnosticStatus;
                    const bool diagnosticP7Enabled = dxrSettings.IsRestirGiSpatialEnabled();
                    const char* diagnosticButtonLabel = diagnosticP7Enabled
                        ? "Write P7 ON diagnostic report##pt-restir-gi"
                        : "Write P7 OFF diagnostic report##pt-restir-gi";
                    if (ImGui::Button(diagnosticButtonLabel))
                    {
                        const char* diagnosticPath = diagnosticP7Enabled
                            ? "diagnostics/pt_restir_gi_p7_on.txt"
                            : "diagnostics/pt_restir_gi_p7_off.txt";
                        RenderDiagnostics::WriteReport(
                            scene,
                            ctx.camera,
                            ctx.viewportWidth,
                            ctx.viewportHeight,
                            diagnosticPath,
                            p7DiagnosticStatus);
                    }
                    if (!p7DiagnosticStatus.empty())
                    {
                        LightingPanelUi::DrawWrappedHelp(p7DiagnosticStatus.c_str());
                    }
                    ImGui::Unindent();
                }
            }
        }

        bool debugTraceEnabled = dxrSettings.IsDebugTraceEnabled();
        UndoableRendererCheckbox(
            "Enable RT debug trace",
            &debugTraceEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDebugTraceEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });

        if (pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote("Hybrid RT effects are handled by the path tracer.");
            ImGui::BeginDisabled();
        }

        bool reflectionsEnabled = dxrSettings.IsReflectionsEnabled();
        UndoableRendererCheckbox(
            "Enable RT reflections",
            &reflectionsEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetReflectionsEnabled(enabled);
                target.MarkDirty();
            });

        RendererSettingUi::MarkRendered("rt_reflections_enabled");

        int qualityIndex = static_cast<int>(dxrSettings.GetReflectionsQuality());
        const char* qualityLabels[] = {"Low", "Medium", "High"};
        if (ImGui::Combo("Reflections quality", &qualityIndex, qualityLabels, IM_ARRAYSIZE(qualityLabels)))
        {
            const auto quality = static_cast<DxrReflectionsQuality>(qualityIndex);
            RendererSettingUi::ApplyChange(
                "rt_reflections_quality",
                editContext,
                scene,
                "RT reflections quality",
                [quality](Scene& target) {
                    target.GetRenderer().GetDxrSettings().SetReflectionsQuality(quality);
                    target.MarkDirty();
                });
        }

        RendererSettingUi::MarkRendered("rt_reflections_quality");

        int samplesPerPixel = dxrSettings.GetReflectionsSamplesPerPixel();
        UndoableRendererSliderInt(
            "Reflection samples / pixel",
            &samplesPerPixel,
            1,
            16,
            editContext,
            [](Scene& target, int samples) {
                target.GetRenderer().GetDxrSettings().SetReflectionsSamplesPerPixel(samples);
                target.MarkDirty();
            });

        float maxTraceDistance = dxrSettings.GetMaxTraceDistance();
        UndoableRendererSliderFloat(
            "Max trace distance",
            &maxTraceDistance,
            1.0f,
            500.0f,
            "%.1f m",
            editContext,
            [](Scene& target, float distance) {
                target.GetRenderer().GetDxrSettings().SetMaxTraceDistance(distance);
                target.MarkDirty();
            });

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("Reflection denoise (NRD RELAX): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool denoiseEnabled = dxrSettings.IsDenoiseEnabled();
        UndoableRendererCheckbox(
            "Denoise enabled",
            &denoiseEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDenoiseEnabled(enabled);
                target.MarkDirty();
            });

        float temporalBlend = dxrSettings.GetTemporalBlend();
        UndoableRendererSliderFloat(
            "Temporal blend",
            &temporalBlend,
            0.0f,
            0.99f,
            "%.2f",
            editContext,
            [](Scene& target, float blend) {
                target.GetRenderer().GetDxrSettings().SetTemporalBlend(blend);
                target.MarkDirty();
            });

        int atrousIterations = dxrSettings.GetReflectionAtrousIterations();
        UndoableRendererSliderInt(
            "Denoiser smoothing (A-trous)",
            &atrousIterations,
            2,
            8,
            editContext,
            [](Scene& target, int iterations) {
                target.GetRenderer().GetDxrSettings().SetReflectionAtrousIterations(iterations);
                target.MarkDirty();
            });

        bool antiFirefly = dxrSettings.IsReflectionAntiFireflyEnabled();
        UndoableRendererCheckbox(
            "Denoiser anti-firefly",
            &antiFirefly,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetReflectionAntiFireflyEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        int aoRays = dxrSettings.GetReflectionAoRays();
        UndoableRendererSliderInt(
            "Reflection AO rays",
            &aoRays,
            0,
            16,
            editContext,
            [](Scene& target, int rays) {
                target.GetRenderer().GetDxrSettings().SetReflectionAoRays(rays);
                target.MarkDirty();
            });
        LightingPanelUi::DrawWrappedNote(
            "Contact shadows on reflected surfaces. 0 = off; higher = cleaner, costlier.");

        float roughnessCutoff = dxrSettings.GetReflectionRoughnessCutoff();
        UndoableRendererSliderFloat(
            "Reflection roughness cutoff",
            &roughnessCutoff,
            0.0f,
            1.0f,
            "%.2f",
            editContext,
            [](Scene& target, float cutoff) {
                target.GetRenderer().GetDxrSettings().SetReflectionRoughnessCutoff(cutoff);
                target.MarkDirty();
            });
        LightingPanelUi::DrawWrappedNote(
            "Surfaces rougher than this skip the RT trace and use IBL (cheaper, less blur).");

        // Phase D8 — RT soft sun shadows (devdoc/dxr/shadows.md). Supplemental over CSM.
        ImGui::SeparatorText("RT shadows");

        bool shadowsEnabled = dxrSettings.IsShadowsEnabled();
        UndoableRendererCheckbox(
            "Enable RT shadows",
            &shadowsEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetShadowsEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });

        RendererSettingUi::MarkRendered("rt_shadows_enabled");

        float sunAngularRadius = dxrSettings.GetSunAngularRadiusDegrees();
        UndoableRendererSliderFloat(
            "Sun angular radius",
            &sunAngularRadius,
            0.05f,
            2.0f,
            "%.2f deg",
            editContext,
            [](Scene& target, float degrees) {
                target.GetRenderer().GetDxrSettings().SetSunAngularRadiusDegrees(degrees);
                target.MarkDirty();
            });
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Angular radius of the sun disk. Drives RT shadow penumbra and path-traced soft sun NEE.");
        }

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("Shadow denoise (NRD SIGMA): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool shadowDenoise = dxrSettings.IsShadowDenoiseEnabled();
        UndoableRendererCheckbox(
            "Shadow denoise (SIGMA)",
            &shadowDenoise,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetShadowDenoiseEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        // Phase D9 — RT diffuse GI. Mutually exclusive with SSGI inject.
        ImGui::SeparatorText("RT diffuse GI");

        const bool ssgiBlocksRtGi = screenSpaceEffects.IsSsgiEnabled();
        if (ssgiBlocksRtGi)
        {
            LightingPanelUi::DrawWrappedNote("SSGI is enabled. Disable SSGI before enabling RT diffuse GI.");
        }

        if (ssgiBlocksRtGi)
        {
            ImGui::BeginDisabled();
        }

        bool giEnabled = dxrSettings.IsGiEnabled();
        UndoableRendererCheckbox(
            "Enable RT GI",
            &giEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetGiEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });
        if (ssgiBlocksRtGi)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Mutually exclusive with SSGI inject.");
            }
        }

        RendererSettingUi::MarkRendered("rt_gi_enabled");

        float giStrength = dxrSettings.GetGiStrength();
        UndoableRendererSliderFloat(
            "GI strength",
            &giStrength,
            0.0f,
            2.0f,
            "%.2f",
            editContext,
            [](Scene& target, float strength) {
                target.GetRenderer().GetDxrSettings().SetGiStrength(strength);
                target.MarkDirty();
            });

        if (rrActive)
        {
            LightingPanelUi::DrawWrappedNote("GI denoise (NRD RELAX): handled by Ray Reconstruction.");
            ImGui::BeginDisabled();
        }
        bool giDenoise = dxrSettings.IsGiDenoiseEnabled();
        UndoableRendererCheckbox(
            "GI denoise (RELAX)",
            &giDenoise,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetGiDenoiseEnabled(enabled);
                target.MarkDirty();
            });
        if (rrActive)
        {
            ImGui::EndDisabled();
        }

        if (pathTracingActive)
        {
            ImGui::EndDisabled();
        }

        LightingPanelUi::DrawWrappedNote(
            "Additive over ambient. Lower Environment intensity if the scene washes out. Overrides SSGI when enabled.");

        ImGui::PopID();

        const DxrDiagnostics& dxrDiagnostics = renderer.GetDxrDiagnostics();
        ImGui::Separator();
        ImGui::Text("BLAS count: %u", dxrDiagnostics.blasCount);
        ImGui::Text("TLAS instances: %u", dxrDiagnostics.tlasInstanceCount);
        ImGui::Text("RT triangles (unique): %llu", static_cast<unsigned long long>(dxrDiagnostics.totalRtTriangles));
        const double asMemoryMb =
            static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / (1024.0 * 1024.0);
        if (asMemoryMb >= 1.0)
        {
            ImGui::Text("AS GPU memory: %.2f MB", asMemoryMb);
        }
        else
        {
            ImGui::Text(
                "AS GPU memory: %.1f KB",
                static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / 1024.0);
        }
        ImGui::Text("Last build status: %s", dxrDiagnostics.buildStatus.c_str());
        ImGui::Text("Last build time: %.3f ms", dxrDiagnostics.lastBuildTimeMs);
        ImGui::Text(
            "Emissive NEE lights: %u  (tris: %u)",
            dxrDiagnostics.emissiveLightCount,
            dxrDiagnostics.emissiveTriangleCount);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Emitters registered for path-tracer emissive NEE (refreshed each PT frame).\n"
                "0 = NEE toward emitters is OFF, so the 'PT isolate: emissive NEE' view is\n"
                "legitimately black and emitter light only arrives via random BSDF hits.\n"
                "Non-zero = NEE is active; a dark isolate view just means few visible surfaces\n"
                "directly face the emitter.");
        }

        if (!raytracingSupported)
        {
            ImGui::EndDisabled();
        }

        LightingPanelUi::DrawWrappedNote(
            "Acceleration structures build when ray tracing is enabled. DispatchRays arrives in a later DXR phase.");
    }
}
