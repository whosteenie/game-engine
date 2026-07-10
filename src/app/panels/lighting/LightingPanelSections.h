#pragma once

#include "app/panels/lighting/LightingPanelContext.h"

void DrawSceneSection(const LightingPanelContext& ctx);
void DrawEnvironmentSection(const LightingPanelContext& ctx);
void DrawDirectionalShadowsSection(const LightingPanelContext& ctx);
void DrawPostProcessingSection(const LightingPanelContext& ctx);
void DrawAmbientOcclusionSection(const LightingPanelContext& ctx);
void DrawSsgiSection(const LightingPanelContext& ctx);
void DrawSsrSection(const LightingPanelContext& ctx);
void DrawRayTracingSection(const LightingPanelContext& ctx);
void DrawAntiAliasingSection(const LightingPanelContext& ctx);
void DrawTextureFilteringSection(const LightingPanelContext& ctx);
void DrawDiagnosticsSection(const LightingPanelContext& ctx);
