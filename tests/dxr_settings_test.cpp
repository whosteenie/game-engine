#include "engine/rendering/DxrSettings.h"

#include <nlohmann/json.hpp>

void RunDxrSettingsTests(int& failures)
{
    auto expectTrue = [&failures](const bool condition, const char* message) {
        if (!condition)
        {
            ++failures;
        }
    };

    DxrSettings settings;
    expectTrue(
        !settings.IsPtOpticalMotionReplayEnabled(),
        "optical motion replay defaults off");
    settings.SetEnabled(true);
    settings.SetReflectionsEnabled(true);
    settings.ClampToHardwareCapabilities(false);
    expectTrue(!settings.IsEnabled(), "enabled forced off when RT unsupported");
    expectTrue(!settings.IsReflectionsEnabled(), "reflections forced off when RT unsupported");

    settings.SetEnabled(true);
    settings.SetReflectionsSamplesPerPixel(32);
    settings.SetMaxTraceDistance(1000.0f);
    settings.SetTemporalBlend(2.0f);
    settings.SetRestirDiCandidateCount(4);
    settings.SetRestirDiTemporalEnabled(true);
    settings.SetRestirGiInitialEnabled(true);
    settings.SetRestirGiTemporalEnabled(true);
    settings.SetRestirGiSpatialEnabled(true);
    settings.SetRestirGiDiagnosticOrbitRevolutions(13);
    settings.SetPtDeterministicOpticalSplitEnabled(true);
    settings.SetPtOpticalMotionReplayEnabled(true);
    settings.ClampToHardwareCapabilities(true);
    expectTrue(settings.IsEnabled(), "enabled preserved when RT supported");
    expectTrue(settings.GetReflectionsSamplesPerPixel() == 16, "samples clamped to 16");
    expectTrue(settings.GetMaxTraceDistance() == 500.0f, "max trace distance clamped to 500");
    expectTrue(settings.GetTemporalBlend() == 0.99f, "temporal blend clamped to 0.99");

    DxrSettings roundTrip;
    roundTrip.ApplyFromJson(settings.ToJson());
    expectTrue(roundTrip.IsEnabled() == settings.IsEnabled(), "json round-trip enabled");
    expectTrue(
        roundTrip.GetRenderingMode() == settings.GetRenderingMode(),
        "json round-trip rendering mode");
    expectTrue(
        roundTrip.GetReflectionsSamplesPerPixel() == settings.GetReflectionsSamplesPerPixel(),
        "json round-trip reflection samples");
    expectTrue(
        roundTrip.GetGiStrength() == settings.GetGiStrength(),
        "json round-trip gi strength");
    expectTrue(roundTrip.GetRestirDiCandidateCount() == 4, "json round-trip ReSTIR DI candidates");
    expectTrue(roundTrip.IsRestirDiTemporalEnabled(), "json round-trip ReSTIR DI temporal toggle");
    expectTrue(roundTrip.IsRestirGiInitialEnabled(), "json round-trip ReSTIR GI initial toggle");
    expectTrue(roundTrip.IsRestirGiTemporalEnabled(), "json round-trip ReSTIR GI temporal toggle");
    expectTrue(roundTrip.IsRestirGiSpatialEnabled(), "json round-trip ReSTIR GI spatial toggle");
    expectTrue(
        roundTrip.IsPtDeterministicOpticalSplitEnabled(),
        "json round-trip deterministic optical split toggle");
    expectTrue(
        roundTrip.IsPtOpticalMotionReplayEnabled(),
        "json round-trip optical motion replay toggle");
    expectTrue(
        roundTrip.GetRestirGiDiagnosticOrbitRevolutions() == 13,
        "json round-trip ReSTIR GI diagnostic orbit revolutions");
}
