#include "engine/rendering/RenderDebug.h"

const char* RenderDebugModeLabel(RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::None:
        return "None (final image)";
    case RenderDebugMode::ShadowFactor:
        return "Shadow factor (PBR)";
    case RenderDebugMode::DirectLighting:
        return "Direct lighting (PBR)";
    case RenderDebugMode::AmbientIbl:
        return "Ambient / IBL (PBR)";
    case RenderDebugMode::LightSpaceUv:
        return "Light-space UV (PBR)";
    case RenderDebugMode::LightSpaceDepth:
        return "Light-space depth (PBR)";
    case RenderDebugMode::CascadeIndex:
        return "Cascade index (PBR)";
    case RenderDebugMode::GeometricNormal:
        return "Geometric normal (PBR)";
    case RenderDebugMode::TangentHandedness:
        return "Tangent handedness (PBR)";
    case RenderDebugMode::ViewDepth:
        return "View depth (PBR)";
    case RenderDebugMode::CascadeBlendFactor:
        return "Cascade blend factor (PBR)";
    case RenderDebugMode::DiffuseIbl:
        return "Diffuse IBL (shaded normal)";
    case RenderDebugMode::SpecularIbl:
        return "Specular IBL (PBR)";
    case RenderDebugMode::DirectDiffuseGeom:
        return "Direct diffuse (geom N·L)";
    case RenderDebugMode::ShadedNormal:
        return "Shaded normal (PBR)";
    case RenderDebugMode::ShadowFactorUnbiased:
        return "Shadow factor (unbiased)";
    case RenderDebugMode::ShadowMapStoredDepth:
        return "Shadow map stored depth";
    case RenderDebugMode::ShadowDepthSeparation:
        return "Shadow depth separation";
    case RenderDebugMode::Ssao:
        return "SSAO buffer";
    case RenderDebugMode::GtaoRaw:
        return "GTAO raw";
    case RenderDebugMode::GtaoFiltered:
        return "GTAO filtered";
    case RenderDebugMode::CompositeOcclusion:
        return "Composite occlusion (AO)";
    case RenderDebugMode::GeomSunFacing:
        return "Geom sun facing (N·L)";
    case RenderDebugMode::ShadowCompareDepth:
        return "Shadow compare depth";
    case RenderDebugMode::ShadowBlockedCenter:
        return "Shadow blocked (center texel)";
    case RenderDebugMode::MotionVectors:
        return "Motion vectors";
    case RenderDebugMode::GBufferAlbedo:
        return "G-buffer albedo";
    case RenderDebugMode::GBufferRoughness:
        return "G-buffer roughness";
    case RenderDebugMode::GBufferMetallic:
        return "G-buffer metallic";
    case RenderDebugMode::GBufferEmissive:
        return "G-buffer emissive";
    case RenderDebugMode::RadianceBuffer:
        return "Radiance buffer";
    case RenderDebugMode::RadianceValidity:
        return "Radiance validity";
    case RenderDebugMode::RadianceTemporal:
        return "Radiance temporal";
    case RenderDebugMode::GiDisocclusion:
        return "GI disocclusion";
    case RenderDebugMode::RadianceTemporalDelta:
        return "Radiance temporal delta";
    case RenderDebugMode::SsgiTraceRaw:
        return "SSGI trace raw";
    case RenderDebugMode::SsgiDenoiseSpatial:
        return "SSGI denoise spatial";
    case RenderDebugMode::SsgiDenoiseTemporal:
        return "SSGI denoise temporal";
    case RenderDebugMode::SsgiDenoiseFinal:
        return "SSGI denoise final";
    case RenderDebugMode::SsgiInject:
        return "SSGI inject";
    case RenderDebugMode::SsgiTraceHitMask:
        return "SSGI trace hit mask";
    case RenderDebugMode::SsgiTraceHitDistance:
        return "SSGI trace confidence";
    case RenderDebugMode::SsgiFinalContribution:
        return "SSGI final contribution";
    case RenderDebugMode::SsrSceneColor:
        return "SSR scene color";
    case RenderDebugMode::SsrSceneValidity:
        return "SSR scene validity";
    case RenderDebugMode::SsrTraceRaw:
        return "SSR trace raw";
    case RenderDebugMode::SsrTraceConfidence:
        return "SSR trace confidence";
    case RenderDebugMode::SsrDenoiseSpatial:
        return "SSR denoise spatial";
    case RenderDebugMode::SsrDenoiseTemporal:
        return "SSR denoise temporal";
    case RenderDebugMode::SsrDenoiseFinal:
        return "SSR denoise final";
    case RenderDebugMode::SsrSvgfVariance:
        return "SSR SVGF variance";
    case RenderDebugMode::SsrUpscaled:
        return "SSR upscaled";
    case RenderDebugMode::SsrSpecReplacement:
        return "SSR spec replacement";
    case RenderDebugMode::RtDispatchSmoke:
        return "RT dispatch smoke";
    case RenderDebugMode::RtPrimaryHit:
        return "RT primary hit";
    case RenderDebugMode::RtPrimaryDepth:
        return "RT primary depth";
    case RenderDebugMode::RtPrimaryNormal:
        return "RT primary normal";
    case RenderDebugMode::RtReflectionRaw:
        return "RT reflection raw";
    case RenderDebugMode::RtReflectionConfidence:
        return "RT reflection hit distance";
    case RenderDebugMode::RtReflectionDenoised:
        return "RT reflection denoised";
    case RenderDebugMode::RtSpecReplacement:
        return "RT spec replacement";
    case RenderDebugMode::RtShadowRaw:
        return "RT shadow raw";
    case RenderDebugMode::RtShadowDenoised:
        return "RT shadow denoised";
    case RenderDebugMode::RtGiRaw:
        return "RT GI raw";
    case RenderDebugMode::RtGiDenoised:
        return "RT GI denoised";
    case RenderDebugMode::RtGiInject:
        return "RT GI inject";
    case RenderDebugMode::RrDiffuseAlbedo:
        return "RR guide: diffuse albedo";
    case RenderDebugMode::RrSpecularAlbedo:
        return "RR guide: specular albedo";
    case RenderDebugMode::RrNormalRoughness:
        return "RR guide: normal-roughness";
    case RenderDebugMode::RrTransmissionDiffuseAlbedo:
        return "RR transmission guide: diffuse albedo";
    case RenderDebugMode::RrTransmissionSpecularAlbedo:
        return "RR transmission guide: specular albedo";
    case RenderDebugMode::RrTransmissionNormalRoughness:
        return "RR transmission guide: normal-roughness";
    case RenderDebugMode::PtOpticalRawReflection:
        return "PT optical RR: raw reflection input";
    case RenderDebugMode::PtOpticalRawTransmission:
        return "PT optical RR: raw transmission input";
    case RenderDebugMode::PtOpticalReconstructedReflection:
        return "PT optical RR: reconstructed reflection";
    case RenderDebugMode::PtOpticalReconstructedTransmission:
        return "PT optical RR: reconstructed transmission";
    case RenderDebugMode::PtOpticalReflectionReconstructionDelta:
        return "PT optical RR: reflection reconstruction delta";
    case RenderDebugMode::PtOpticalTransmissionReconstructionDelta:
        return "PT optical RR: transmission reconstruction delta";
    case RenderDebugMode::PtIsolateDirectSun:
        return "PT isolate: direct sun";
    case RenderDebugMode::PtIsolateDirectEmissive:
        return "PT isolate: emissive NEE";
    case RenderDebugMode::PtIsolateSurfaceEmissive:
        return "PT isolate: surface emissive";
    case RenderDebugMode::PtIsolateAmbient:
        return "PT isolate: SH ambient";
    case RenderDebugMode::PtIsolateAoVisibility:
        return "PT isolate: AO visibility";
    case RenderDebugMode::PtIsolateSunVisibility:
        return "PT isolate: sun visibility";
    case RenderDebugMode::PtIsolateIndirect:
        return "PT isolate: indirect only";
    case RenderDebugMode::PtIsolatePreClamp:
        return "PT isolate: pre-clamp radiance";
    case RenderDebugMode::PtIsolateSpecHitDist:
        return "PT isolate: spec hit distance";
    case RenderDebugMode::PtRestirLinearDepth:
        return "PT ReSTIR surface: linear depth";
    case RenderDebugMode::PtRestirGeometricNormal:
        return "PT ReSTIR surface: geometric normal";
    case RenderDebugMode::PtRestirMaterialId:
        return "PT ReSTIR surface: material ID";
    case RenderDebugMode::PtRestirLobeClass:
        return "PT ReSTIR surface: lobe class";
    case RenderDebugMode::PtRestirReservoirM: return "PT ReSTIR DI: reservoir M";
    case RenderDebugMode::PtRestirReservoirAge: return "PT ReSTIR DI: reservoir age";
    case RenderDebugMode::PtRestirChosenSource: return "PT ReSTIR DI: chosen source";
    case RenderDebugMode::PtRestirTemporalRejection: return "PT ReSTIR DI: temporal rejection";
    case RenderDebugMode::PtRestirSpatialSource: return "PT ReSTIR DI: spatial source";
    case RenderDebugMode::PtRestirSpatialRejection: return "PT ReSTIR DI: spatial rejection";
    case RenderDebugMode::PtRestirGiReservoirM: return "PT ReSTIR GI: reservoir M";
    case RenderDebugMode::PtRestirGiReservoirAge: return "PT ReSTIR GI: reservoir age";
    case RenderDebugMode::PtRestirGiChosenSource: return "PT ReSTIR GI: chosen source";
    case RenderDebugMode::PtRestirGiTemporalRejection: return "PT ReSTIR GI: temporal rejection";
    case RenderDebugMode::PtRestirGiUcw: return "PT ReSTIR GI: UCW / weight";
    case RenderDebugMode::PtRestirGiContribution: return "PT ReSTIR GI: contribution";
    case RenderDebugMode::PtRestirGiReuseMinusFresh: return "PT ReSTIR GI: reuse - fresh (bias)";
    case RenderDebugMode::PtRestirGiReusedRadiance: return "PT ReSTIR GI: reused radiance";
    case RenderDebugMode::PtEnvDiProbeSampling: return "PT perf probe: env DI sampling";
    case RenderDebugMode::PtEnvDiProbeBsdfMis: return "PT perf probe: env DI sampling + BSDF/MIS";
    case RenderDebugMode::PtEnvDiProbeCandidate: return "PT perf probe: env DI candidate (no reservoir)";
    case RenderDebugMode::PtEnvDiProbeRadiance: return "PT perf probe: env DI radiance (no metadata/reservoir)";
    case RenderDebugMode::PtEnvDiProbeMetadata: return "PT perf probe: env DI metadata (no radiance/reservoir)";
    case RenderDebugMode::PtRestirGiSpatialSource: return "PT ReSTIR GI: spatial source";
    case RenderDebugMode::PtRestirGiSpatialRejection: return "PT ReSTIR GI: spatial rejection";
    case RenderDebugMode::PtRestirGiSpatialUcw: return "PT ReSTIR GI: post-spatial UCW";
    case RenderDebugMode::PtRestirGiSpatialContribution: return "PT ReSTIR GI: post-spatial contribution";
    case RenderDebugMode::PtRestirGiSpatialDelta: return "PT ReSTIR GI: spatial - temporal";
    case RenderDebugMode::PtRestirGiSpatialEffectiveWeight: return "PT ReSTIR GI: effective reservoir weight";
    case RenderDebugMode::PtRestirGiSpatialJacobian: return "PT ReSTIR GI: selected Jacobian";
    case RenderDebugMode::PtRestirGiSpatialNormalization: return "PT ReSTIR GI: BASIC normalization";
    case RenderDebugMode::PtRestirGiSpatialMisWeights: return "PT ReSTIR GI: final MIS weights";
    case RenderDebugMode::PtRestirGiSpatialSupport: return "PT ReSTIR GI: spatial support";
    case RenderDebugMode::PtRestirGiSpatialFilterScore: return "PT ReSTIR GI: outlier-filter score";
    case RenderDebugMode::PtRestirGiSpatialStaticVariance: return "PT ReSTIR GI: static variance";
    case RenderDebugMode::PtRestirGiSpatialMotionDelta: return "PT ReSTIR GI: motion-reprojected delta";
    case RenderDebugMode::PtTemporalRelativeSigma:
        return "PT temporal: relative sigma";
    case RenderDebugMode::PtTemporalFrameDelta:
        return "PT temporal: frame delta";
    case RenderDebugMode::PtMotionReprojectionResidual:
        return "PT/DLSS motion: reprojection residual";
    case RenderDebugMode::PtDepthReprojectionDisocclusion:
        return "PT/DLSS depth: reprojection disocclusion";
    case RenderDebugMode::PtMatrixDepthReprojectionDisocclusion:
        return "PT/DLSS matrix: depth reprojection disocclusion";
    case RenderDebugMode::PtCameraOpaqueMotion:
        return "PT camera-domain: opaque motion";
    case RenderDebugMode::PtTransmissionVirtualMotion:
        return "PT camera-domain: transmission virtual motion";
    case RenderDebugMode::PtRestirPreviousReceiverTargetAgreement:
        return "PT ReSTIR: previous receiver/target agreement";
    case RenderDebugMode::PtOpticalInterfaceNormal:
        return "PT optical: smooth interface normal";
    case RenderDebugMode::PtOpticalInterfaceEvent:
        return "PT optical: sampled interface event";
    case RenderDebugMode::PtOpticalReflectCandidate:
        return "PT optical: reflection candidate";
    case RenderDebugMode::PtOpticalRefractCandidate:
        return "PT optical: refraction candidate";
    case RenderDebugMode::PtOpticalGuideReceiverId:
        return "PT optical: guide receiver ID";
    case RenderDebugMode::PtOpticalGuideFallback:
        return "PT optical: guide fallback policy";
    case RenderDebugMode::PtOpticalReceiverReprojection:
        return "PT optical: receiver reprojection residual";
    case RenderDebugMode::PtOpticalCoverageFresnel:
        return "PT optical: coverage / Fresnel weights";
    case RenderDebugMode::PtOpticalReflectionReprojection:
        return "PT optical: reflection reprojection residual";
    case RenderDebugMode::PtOpticalTransmissionReprojection:
        return "PT optical: transmission reprojection residual";
    case RenderDebugMode::PtOpticalReflectionReplayStatus:
        return "PT optical: reflection replay status";
    case RenderDebugMode::PtOpticalTransmissionReplayStatus:
        return "PT optical: transmission replay status";
    case RenderDebugMode::PtOpticalTransmissionAttribution:
        return "PT optical: transmission tail attribution";
    case RenderDebugMode::PtOpticalTransmissionEnvironment:
        return "PT optical: transmission environment";
    case RenderDebugMode::PtOpticalTransmissionReceiver:
        return "PT optical: transmission first receiver";
    case RenderDebugMode::PtOpticalTransmissionDeepBounce:
        return "PT optical: transmission deep bounce";
    default:
        return "Unknown";
    }
}

bool IsPbrMaterialDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::ShadowFactor:
    case RenderDebugMode::DirectLighting:
    case RenderDebugMode::AmbientIbl:
    case RenderDebugMode::LightSpaceUv:
    case RenderDebugMode::LightSpaceDepth:
    case RenderDebugMode::CascadeIndex:
    case RenderDebugMode::GeometricNormal:
    case RenderDebugMode::TangentHandedness:
    case RenderDebugMode::ViewDepth:
    case RenderDebugMode::CascadeBlendFactor:
    case RenderDebugMode::DiffuseIbl:
    case RenderDebugMode::SpecularIbl:
    case RenderDebugMode::DirectDiffuseGeom:
    case RenderDebugMode::ShadedNormal:
    case RenderDebugMode::ShadowFactorUnbiased:
    case RenderDebugMode::ShadowMapStoredDepth:
    case RenderDebugMode::ShadowDepthSeparation:
    case RenderDebugMode::GeomSunFacing:
    case RenderDebugMode::ShadowCompareDepth:
    case RenderDebugMode::ShadowBlockedCenter:
        return true;
    default:
        return false;
    }
}

bool IsGBufferDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::GBufferAlbedo:
    case RenderDebugMode::GBufferRoughness:
    case RenderDebugMode::GBufferMetallic:
    case RenderDebugMode::GBufferEmissive:
        return true;
    default:
        return false;
    }
}

bool IsRadianceDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RadianceBuffer:
    case RenderDebugMode::RadianceValidity:
        return true;
    default:
        return false;
    }
}

bool IsGiTemporalDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RadianceTemporal:
    case RenderDebugMode::GiDisocclusion:
    case RenderDebugMode::RadianceTemporalDelta:
        return true;
    default:
        return false;
    }
}

bool IsSsgiDenoiseDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsgiTraceRaw:
    case RenderDebugMode::SsgiDenoiseSpatial:
    case RenderDebugMode::SsgiDenoiseTemporal:
    case RenderDebugMode::SsgiDenoiseFinal:
        return true;
    case RenderDebugMode::SsgiInject:
    case RenderDebugMode::SsgiTraceHitMask:
    case RenderDebugMode::SsgiTraceHitDistance:
    case RenderDebugMode::SsgiFinalContribution:
        return true;
    default:
        return false;
    }
}

bool IsSsrDebugMode(const RenderDebugMode mode)
{
    return IsSsrSceneDebugMode(mode) || IsSsrTraceDebugMode(mode) || IsSsrDenoiseDebugMode(mode)
        || IsSsrCompositeDebugMode(mode);
}

bool IsSsrSceneDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrSceneColor:
    case RenderDebugMode::SsrSceneValidity:
        return true;
    default:
        return false;
    }
}

bool IsSsrTraceDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrTraceRaw:
    case RenderDebugMode::SsrTraceConfidence:
        return true;
    default:
        return false;
    }
}

bool IsSsrDenoiseDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrDenoiseSpatial:
    case RenderDebugMode::SsrDenoiseTemporal:
    case RenderDebugMode::SsrDenoiseFinal:
    case RenderDebugMode::SsrSvgfVariance:
    case RenderDebugMode::SsrUpscaled:
        return true;
    default:
        return false;
    }
}

bool IsSsrCompositeDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::SsrSpecReplacement;
}

bool IsDxrDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::RtDispatchSmoke || IsRtPrimaryDebugMode(mode)
        || IsRtReflectionDebugMode(mode) || IsRtShadowDebugMode(mode) || IsRtGiDebugMode(mode);
}

bool IsRtGiDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtGiRaw:
    case RenderDebugMode::RtGiDenoised:
    case RenderDebugMode::RtGiInject:
        return true;
    default:
        return false;
    }
}

bool IsRrGuideDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RrDiffuseAlbedo:
    case RenderDebugMode::RrSpecularAlbedo:
    case RenderDebugMode::RrNormalRoughness:
    case RenderDebugMode::RrTransmissionDiffuseAlbedo:
    case RenderDebugMode::RrTransmissionSpecularAlbedo:
    case RenderDebugMode::RrTransmissionNormalRoughness:
        return true;
    default:
        return false;
    }
}

bool IsPtOpticalLayerDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtOpticalRawReflection:
    case RenderDebugMode::PtOpticalRawTransmission:
    case RenderDebugMode::PtOpticalReconstructedReflection:
    case RenderDebugMode::PtOpticalReconstructedTransmission:
    case RenderDebugMode::PtOpticalReflectionReconstructionDelta:
    case RenderDebugMode::PtOpticalTransmissionReconstructionDelta:
        return true;
    default:
        return false;
    }
}

bool IsPtIsolateDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtIsolateDirectSun:
    case RenderDebugMode::PtIsolateDirectEmissive:
    case RenderDebugMode::PtIsolateSurfaceEmissive:
    case RenderDebugMode::PtIsolateAmbient:
    case RenderDebugMode::PtIsolateAoVisibility:
    case RenderDebugMode::PtIsolateSunVisibility:
    case RenderDebugMode::PtIsolateIndirect:
    case RenderDebugMode::PtIsolatePreClamp:
    case RenderDebugMode::PtIsolateSpecHitDist:
    case RenderDebugMode::PtRestirLinearDepth:
    case RenderDebugMode::PtRestirGeometricNormal:
    case RenderDebugMode::PtRestirMaterialId:
    case RenderDebugMode::PtRestirLobeClass:
    case RenderDebugMode::PtRestirReservoirM:
    case RenderDebugMode::PtRestirReservoirAge:
    case RenderDebugMode::PtRestirChosenSource:
    case RenderDebugMode::PtRestirTemporalRejection:
    case RenderDebugMode::PtRestirSpatialSource:
    case RenderDebugMode::PtRestirSpatialRejection:
    case RenderDebugMode::PtRestirGiReservoirM:
    case RenderDebugMode::PtRestirGiReservoirAge:
    case RenderDebugMode::PtRestirGiChosenSource:
    case RenderDebugMode::PtRestirGiTemporalRejection:
    case RenderDebugMode::PtRestirGiUcw:
    case RenderDebugMode::PtRestirGiContribution:
    case RenderDebugMode::PtRestirGiReuseMinusFresh:
    case RenderDebugMode::PtRestirGiReusedRadiance:
    case RenderDebugMode::PtEnvDiProbeSampling:
    case RenderDebugMode::PtEnvDiProbeBsdfMis:
    case RenderDebugMode::PtEnvDiProbeCandidate:
    case RenderDebugMode::PtEnvDiProbeRadiance:
    case RenderDebugMode::PtEnvDiProbeMetadata:
    case RenderDebugMode::PtRestirGiSpatialSource:
    case RenderDebugMode::PtRestirGiSpatialRejection:
    case RenderDebugMode::PtRestirGiSpatialUcw:
    case RenderDebugMode::PtRestirGiSpatialContribution:
    case RenderDebugMode::PtRestirGiSpatialDelta:
    case RenderDebugMode::PtRestirGiSpatialEffectiveWeight:
    case RenderDebugMode::PtRestirGiSpatialJacobian:
    case RenderDebugMode::PtRestirGiSpatialNormalization:
    case RenderDebugMode::PtRestirGiSpatialMisWeights:
    case RenderDebugMode::PtRestirGiSpatialSupport:
    case RenderDebugMode::PtRestirGiSpatialFilterScore:
    case RenderDebugMode::PtRestirGiSpatialStaticVariance:
    case RenderDebugMode::PtRestirGiSpatialMotionDelta:
    case RenderDebugMode::PtCameraOpaqueMotion:
    case RenderDebugMode::PtTransmissionVirtualMotion:
    case RenderDebugMode::PtRestirPreviousReceiverTargetAgreement:
    case RenderDebugMode::PtOpticalInterfaceNormal:
    case RenderDebugMode::PtOpticalInterfaceEvent:
    case RenderDebugMode::PtOpticalReflectCandidate:
    case RenderDebugMode::PtOpticalRefractCandidate:
    case RenderDebugMode::PtOpticalGuideReceiverId:
    case RenderDebugMode::PtOpticalGuideFallback:
    case RenderDebugMode::PtOpticalReceiverReprojection:
    case RenderDebugMode::PtOpticalCoverageFresnel:
    case RenderDebugMode::PtOpticalReflectionReprojection:
    case RenderDebugMode::PtOpticalTransmissionReprojection:
    case RenderDebugMode::PtOpticalReflectionReplayStatus:
    case RenderDebugMode::PtOpticalTransmissionReplayStatus:
    case RenderDebugMode::PtOpticalTransmissionAttribution:
    case RenderDebugMode::PtOpticalTransmissionEnvironment:
    case RenderDebugMode::PtOpticalTransmissionReceiver:
    case RenderDebugMode::PtOpticalTransmissionDeepBounce:
        return true;
    default:
        return false;
    }
}

bool IsPtTemporalStatsDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtTemporalRelativeSigma:
    case RenderDebugMode::PtTemporalFrameDelta:
    case RenderDebugMode::PtRestirGiSpatialStaticVariance:
    case RenderDebugMode::PtRestirGiSpatialMotionDelta:
        return true;
    default:
        return false;
    }
}

bool IsPtRestirGiSpatialDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtRestirGiSpatialSource:
    case RenderDebugMode::PtRestirGiSpatialRejection:
    case RenderDebugMode::PtRestirGiSpatialUcw:
    case RenderDebugMode::PtRestirGiSpatialContribution:
    case RenderDebugMode::PtRestirGiSpatialDelta:
    case RenderDebugMode::PtRestirGiSpatialEffectiveWeight:
    case RenderDebugMode::PtRestirGiSpatialJacobian:
    case RenderDebugMode::PtRestirGiSpatialNormalization:
    case RenderDebugMode::PtRestirGiSpatialMisWeights:
    case RenderDebugMode::PtRestirGiSpatialSupport:
    case RenderDebugMode::PtRestirGiSpatialFilterScore:
    case RenderDebugMode::PtRestirGiSpatialStaticVariance:
    case RenderDebugMode::PtRestirGiSpatialMotionDelta:
        return true;
    default:
        return false;
    }
}

bool IsPtRestirGiSpatialStatsDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::PtRestirGiSpatialStaticVariance
        || mode == RenderDebugMode::PtRestirGiSpatialMotionDelta;
}

bool IsPtMotionReprojectionDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::PtMotionReprojectionResidual;
}

bool IsPtDepthReprojectionDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::PtDepthReprojectionDisocclusion;
}

bool IsPtMatrixDepthReprojectionDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::PtMatrixDepthReprojectionDisocclusion;
}

int PtDebugIsolateModeFromRenderDebug(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtIsolateDirectSun: return 1;
    case RenderDebugMode::PtIsolateDirectEmissive: return 2;
    case RenderDebugMode::PtIsolateSurfaceEmissive: return 3;
    case RenderDebugMode::PtIsolateAmbient: return 4;
    case RenderDebugMode::PtIsolateAoVisibility: return 5;
    case RenderDebugMode::PtIsolateSunVisibility: return 6;
    case RenderDebugMode::PtIsolateIndirect: return 7;
    case RenderDebugMode::PtIsolatePreClamp: return 8;
    case RenderDebugMode::PtIsolateSpecHitDist: return 9;
    case RenderDebugMode::PtRestirLinearDepth: return 10;
    case RenderDebugMode::PtRestirGeometricNormal: return 11;
    case RenderDebugMode::PtRestirMaterialId: return 12;
    case RenderDebugMode::PtRestirLobeClass: return 13;
    case RenderDebugMode::PtRestirReservoirM: return 14;
    case RenderDebugMode::PtRestirReservoirAge: return 15;
    case RenderDebugMode::PtRestirChosenSource: return 16;
    case RenderDebugMode::PtRestirTemporalRejection: return 17;
    case RenderDebugMode::PtRestirSpatialSource: return 18;
    case RenderDebugMode::PtRestirSpatialRejection: return 19;
    case RenderDebugMode::PtRestirGiReservoirM: return 20;
    case RenderDebugMode::PtRestirGiReservoirAge: return 21;
    case RenderDebugMode::PtRestirGiChosenSource: return 22;
    case RenderDebugMode::PtRestirGiTemporalRejection: return 23;
    case RenderDebugMode::PtRestirGiUcw: return 24;
    case RenderDebugMode::PtRestirGiContribution: return 25;
    case RenderDebugMode::PtRestirGiReuseMinusFresh: return 26;
    case RenderDebugMode::PtRestirGiReusedRadiance: return 27;
    case RenderDebugMode::PtEnvDiProbeSampling: return 28;
    case RenderDebugMode::PtEnvDiProbeBsdfMis: return 29;
    case RenderDebugMode::PtEnvDiProbeCandidate: return 30;
    case RenderDebugMode::PtEnvDiProbeRadiance: return 31;
    case RenderDebugMode::PtEnvDiProbeMetadata: return 32;
    case RenderDebugMode::PtRestirGiSpatialSource: return 33;
    case RenderDebugMode::PtRestirGiSpatialRejection: return 34;
    case RenderDebugMode::PtRestirGiSpatialUcw: return 35;
    case RenderDebugMode::PtRestirGiSpatialContribution: return 36;
    case RenderDebugMode::PtRestirGiSpatialDelta: return 37;
    case RenderDebugMode::PtRestirGiSpatialEffectiveWeight: return 38;
    case RenderDebugMode::PtRestirGiSpatialJacobian: return 39;
    case RenderDebugMode::PtRestirGiSpatialNormalization: return 40;
    case RenderDebugMode::PtRestirGiSpatialMisWeights: return 41;
    case RenderDebugMode::PtRestirGiSpatialSupport: return 42;
    case RenderDebugMode::PtRestirGiSpatialFilterScore: return 43;
    case RenderDebugMode::PtRestirGiSpatialStaticVariance: return 44;
    case RenderDebugMode::PtRestirGiSpatialMotionDelta: return 45;
    case RenderDebugMode::PtCameraOpaqueMotion: return 46;
    case RenderDebugMode::PtTransmissionVirtualMotion: return 47;
    case RenderDebugMode::PtRestirPreviousReceiverTargetAgreement: return 48;
    case RenderDebugMode::PtOpticalInterfaceNormal: return 49;
    case RenderDebugMode::PtOpticalInterfaceEvent: return 50;
    case RenderDebugMode::PtOpticalReflectCandidate: return 51;
    case RenderDebugMode::PtOpticalRefractCandidate: return 52;
    case RenderDebugMode::PtOpticalGuideReceiverId: return 53;
    case RenderDebugMode::PtOpticalGuideFallback: return 54;
    case RenderDebugMode::PtOpticalReceiverReprojection: return 55;
    case RenderDebugMode::PtOpticalCoverageFresnel: return 56;
    case RenderDebugMode::PtOpticalReflectionReprojection: return 57;
    case RenderDebugMode::PtOpticalTransmissionReprojection: return 58;
    case RenderDebugMode::PtOpticalReflectionReplayStatus: return 59;
    case RenderDebugMode::PtOpticalTransmissionReplayStatus: return 60;
    case RenderDebugMode::PtOpticalTransmissionAttribution: return 61;
    case RenderDebugMode::PtOpticalTransmissionEnvironment: return 62;
    case RenderDebugMode::PtOpticalTransmissionReceiver: return 63;
    case RenderDebugMode::PtOpticalTransmissionDeepBounce: return 64;
    default: return 0;
    }
}

RenderDebugMode RenderDebugModeFromPtDebugIsolateMode(const int mode)
{
    switch (mode)
    {
    case 28: return RenderDebugMode::PtEnvDiProbeSampling;
    case 29: return RenderDebugMode::PtEnvDiProbeBsdfMis;
    case 30: return RenderDebugMode::PtEnvDiProbeCandidate;
    case 31: return RenderDebugMode::PtEnvDiProbeRadiance;
    case 32: return RenderDebugMode::PtEnvDiProbeMetadata;
    default: return RenderDebugMode::None;
    }
}

bool IsRtShadowDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtShadowRaw:
    case RenderDebugMode::RtShadowDenoised:
        return true;
    default:
        return false;
    }
}

bool IsRtPrimaryDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtPrimaryHit:
    case RenderDebugMode::RtPrimaryDepth:
    case RenderDebugMode::RtPrimaryNormal:
        return true;
    default:
        return false;
    }
}

bool IsRtReflectionDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtReflectionRaw:
    case RenderDebugMode::RtReflectionConfidence:
    case RenderDebugMode::RtReflectionDenoised:
    case RenderDebugMode::RtSpecReplacement:
        return true;
    default:
        return false;
    }
}
