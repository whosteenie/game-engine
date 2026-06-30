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
    GtaoRaw,
    GtaoFiltered,
    CompositeOcclusion,
    GeomSunFacing,
    ShadowCompareDepth,
    ShadowBlockedCenter,
    MotionVectors,
    GBufferAlbedo,
    GBufferRoughness,
    GBufferMetallic,
    GBufferEmissive,
    RadianceBuffer,
    RadianceValidity,
    RadianceTemporal,
    GiDisocclusion,
    RadianceTemporalDelta,
    SsgiTraceRaw,
    SsgiDenoiseSpatial,
    SsgiDenoiseTemporal,
    SsgiDenoiseFinal,
    SsgiInject,
    SsgiTraceHitMask,
    SsgiTraceHitDistance,
    SsgiFinalContribution,
    SsrSceneColor,
    SsrSceneValidity,
    SsrTraceRaw,
    SsrTraceConfidence,
    SsrDenoiseSpatial,
    SsrDenoiseTemporal,
    SsrDenoiseFinal,
    SsrSvgfVariance,
    SsrUpscaled,
    SsrSpecReplacement,
    RtDispatchSmoke,
    RtPrimaryHit,
    RtPrimaryDepth,
    RtPrimaryNormal,
};

bool IsPbrMaterialDebugMode(RenderDebugMode mode);
bool IsGBufferDebugMode(RenderDebugMode mode);
bool IsRadianceDebugMode(RenderDebugMode mode);
bool IsGiTemporalDebugMode(RenderDebugMode mode);
bool IsSsgiDenoiseDebugMode(RenderDebugMode mode);
bool IsSsrDebugMode(RenderDebugMode mode);
bool IsSsrSceneDebugMode(RenderDebugMode mode);
bool IsSsrTraceDebugMode(RenderDebugMode mode);
bool IsSsrDenoiseDebugMode(RenderDebugMode mode);
bool IsSsrCompositeDebugMode(RenderDebugMode mode);
bool IsDxrDebugMode(RenderDebugMode mode);
bool IsRtPrimaryDebugMode(RenderDebugMode mode);

const char* RenderDebugModeLabel(RenderDebugMode mode);
