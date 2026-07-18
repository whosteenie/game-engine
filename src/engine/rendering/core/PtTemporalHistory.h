#pragma once

#include <cstdint>

class DxrSettings;
class EnvironmentMap;

// G1 (ReSTIR R0): fingerprints for inputs that are not covered by DXR geometry upload.
// When either fingerprint changes, DxrAccelerationStructures::BumpPtSceneVersion() is called so
// PT reference accumulation, DLSS-RR motion history, and (later) reservoirs reset together.
std::uint64_t ComputePtEnvironmentFingerprint(const EnvironmentMap& environmentMap);
std::uint64_t ComputePtSettingsFingerprint(const DxrSettings& settings);
