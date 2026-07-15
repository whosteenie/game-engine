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
    RtReflectionRaw,
    RtReflectionConfidence,
    RtReflectionDenoised,
    RtSpecReplacement,
    RtShadowRaw,
    RtShadowDenoised,
    RtGiRaw,
    RtGiDenoised,
    RtGiInject,
    RrDiffuseAlbedo,
    RrSpecularAlbedo,
    RrNormalRoughness,
    // Path-tracer radiance isolation (devdoc/dxr/pt/crevice-darkening.md optional debug views).
    PtIsolateDirectSun,
    PtIsolateDirectEmissive,
    PtIsolateSurfaceEmissive,
    PtIsolateAmbient,
    PtIsolateAoVisibility,
    PtIsolateSunVisibility,
    PtIsolateIndirect,
    PtIsolatePreClamp,
    PtIsolateSpecHitDist,
    PtRestirLinearDepth,
    PtRestirGeometricNormal,
    PtRestirMaterialId,
    PtRestirLobeClass,
    PtRestirReservoirM,
    PtRestirReservoirAge,
    PtRestirChosenSource,
    PtRestirTemporalRejection,
    PtRestirSpatialSource,
    PtRestirSpatialRejection,
    PtRestirGiReservoirM,
    PtRestirGiReservoirAge,
    PtRestirGiChosenSource,
    PtRestirGiTemporalRejection,
    PtRestirGiUcw,
    PtRestirGiContribution,
    PtRestirGiReuseMinusFresh,
    PtRestirGiReusedRadiance,
    PtEnvDiProbeSampling,
    PtEnvDiProbeBsdfMis,
    PtEnvDiProbeCandidate,
    PtEnvDiProbeRadiance,
    PtEnvDiProbeMetadata,
    PtTemporalRelativeSigma,
    PtTemporalFrameDelta,
    PtMotionReprojectionResidual,
    PtDepthReprojectionDisocclusion,
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
bool IsRtReflectionDebugMode(RenderDebugMode mode);
bool IsRtShadowDebugMode(RenderDebugMode mode);
bool IsRtGiDebugMode(RenderDebugMode mode);
bool IsRrGuideDebugMode(RenderDebugMode mode);
bool IsPtIsolateDebugMode(RenderDebugMode mode);
bool IsPtTemporalStatsDebugMode(RenderDebugMode mode);
bool IsPtMotionReprojectionDebugMode(RenderDebugMode mode);
bool IsPtDepthReprojectionDebugMode(RenderDebugMode mode);
int PtDebugIsolateModeFromRenderDebug(RenderDebugMode mode);
RenderDebugMode RenderDebugModeFromPtDebugIsolateMode(int mode);
inline constexpr int kPtDebugIsolateModeMax = 32;

const char* RenderDebugModeLabel(RenderDebugMode mode);
