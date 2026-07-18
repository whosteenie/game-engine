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
    RrTransmissionDiffuseAlbedo,
    RrTransmissionSpecularAlbedo,
    RrTransmissionNormalRoughness,
    // Post-RR optical layer inspection. These modes keep RR active and display the exact layer
    // resources consumed or produced by the independent optical reconstruction evaluations.
    PtOpticalRawReflection,
    PtOpticalRawTransmission,
    PtOpticalReconstructedReflection,
    PtOpticalReconstructedTransmission,
    PtOpticalReflectionReconstructionDelta,
    PtOpticalTransmissionReconstructionDelta,
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
    PtRestirGiSpatialSource,
    PtRestirGiSpatialRejection,
    PtRestirGiSpatialUcw,
    PtRestirGiSpatialContribution,
    PtRestirGiSpatialDelta,
    PtRestirGiSpatialEffectiveWeight,
    PtRestirGiSpatialJacobian,
    PtRestirGiSpatialNormalization,
    PtRestirGiSpatialMisWeights,
    PtRestirGiSpatialSupport,
    PtRestirGiSpatialFilterScore,
    PtRestirGiSpatialStaticVariance,
    PtRestirGiSpatialMotionDelta,
    PtTemporalRelativeSigma,
    PtTemporalFrameDelta,
    PtMotionReprojectionResidual,
    PtDepthReprojectionDisocclusion,
    PtMatrixDepthReprojectionDisocclusion,
    // S1-P2 camera-domain verification. These are gated diagnostic permutations only; they do
    // not alter the PT motion guide or ReSTIR reservoirs.
    PtCameraOpaqueMotion,
    PtTransmissionVirtualMotion,
    PtRestirPreviousReceiverTargetAgreement,
    // Block 1 smooth optical interface diagnostics (display-only PT AOVs).
    PtOpticalInterfaceNormal,
    PtOpticalInterfaceEvent,
    PtOpticalReflectCandidate,
    PtOpticalRefractCandidate,
    PtOpticalGuideReceiverId,
    PtOpticalGuideFallback,
    PtOpticalReceiverReprojection,
    PtOpticalCoverageFresnel,
    PtOpticalReflectionReprojection,
    PtOpticalTransmissionReprojection,
    PtOpticalReflectionReplayStatus,
    PtOpticalTransmissionReplayStatus,
    PtOpticalTransmissionAttribution,
    PtOpticalTransmissionEnvironment,
    PtOpticalTransmissionReceiver,
    PtOpticalTransmissionDeepBounce,
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
bool IsPtOpticalLayerDebugMode(RenderDebugMode mode);
bool IsPtIsolateDebugMode(RenderDebugMode mode);
bool IsPtTemporalStatsDebugMode(RenderDebugMode mode);
bool IsPtRestirGiSpatialDebugMode(RenderDebugMode mode);
bool IsPtRestirGiSpatialStatsDebugMode(RenderDebugMode mode);
bool IsPtMotionReprojectionDebugMode(RenderDebugMode mode);
bool IsPtDepthReprojectionDebugMode(RenderDebugMode mode);
bool IsPtMatrixDepthReprojectionDebugMode(RenderDebugMode mode);
int PtDebugIsolateModeFromRenderDebug(RenderDebugMode mode);
RenderDebugMode RenderDebugModeFromPtDebugIsolateMode(int mode);
inline constexpr int kPtDebugIsolateModeMax = 64;

const char* RenderDebugModeLabel(RenderDebugMode mode);
