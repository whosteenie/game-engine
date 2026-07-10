#include "engine/rendering/PtTemporalHistory.h"

#include "engine/rendering/DxrSettings.h"

#include <cstdlib>
#include <iostream>

namespace
{
    void ExpectTrue(const bool condition, const char* message, int& failures)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }
}

void RunPtTemporalHistoryTests(int& failures)
{
    DxrSettings settingsA{};
    DxrSettings settingsB{};
    settingsB.SetPtMaxBounces(settingsA.GetPtMaxBounces() + 1);

    const std::uint64_t fingerprintA = ComputePtSettingsFingerprint(settingsA);
    const std::uint64_t fingerprintB = ComputePtSettingsFingerprint(settingsB);
    ExpectTrue(fingerprintA != fingerprintB, "PtSettingsFingerprint changes when max bounces changes", failures);
    ExpectTrue(
        fingerprintA == ComputePtSettingsFingerprint(settingsA),
        "PtSettingsFingerprint is stable across calls",
        failures);

    DxrSettings settingsC{};
    settingsC.SetPtAmbientAoRayCount(settingsA.GetPtAmbientAoRayCount() + 1);
    ExpectTrue(
        fingerprintA != ComputePtSettingsFingerprint(settingsC),
        "PtSettingsFingerprint changes when ambient AO ray count changes",
        failures);
}
