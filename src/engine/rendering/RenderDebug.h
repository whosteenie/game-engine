#pragma once

enum class RenderDebugMode
{
    None = 0,
    ShadowFactor,
    DirectLighting,
    AmbientIbl,
    LightSpaceUv,
    LightSpaceDepth,
    CascadeIndex,
    GeometricNormal,
    TangentHandedness,
    ViewDepth,
    CascadeBlendFactor,
    DiffuseIbl,
    SpecularIbl,
    DirectDiffuseGeom,
    ShadedNormal,
    ShadowFactorUnbiased,
    ShadowMapStoredDepth,
    ShadowDepthSeparation,
    Ssao,
    CompositeOcclusion,
    GeomSunFacing,
    ShadowCompareDepth,
    ShadowBlockedCenter,
};

bool IsPbrMaterialDebugMode(RenderDebugMode mode);

const char* RenderDebugModeLabel(RenderDebugMode mode);
