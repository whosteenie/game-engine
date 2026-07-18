#pragma once

#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

float FindGpuPassMilliseconds(const char* passName);
const char* AntiAliasingModeLabel(AntiAliasingMode mode);
const char* DlssPresetLabel(DlssPreset preset);
const char* AmbientOcclusionModeLabel(AmbientOcclusionMode mode);
int IblCubemapResolutionToComboIndex(EnvironmentIblCubemapResolution resolution);
EnvironmentIblCubemapResolution IblCubemapResolutionFromComboIndex(int index);
