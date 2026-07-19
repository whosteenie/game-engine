// DXR path tracer — Phase P2 core integrator (devdoc/dxr/path-tracing.md).
//
// Megakernel: the raygen owns the bounce loop (throughput + NEE + BRDF sampling + Russian roulette).
// Closest-hit only extracts surface data; shadow and bounce traces originate from raygen so
// MaxTraceRecursionDepth = 1 suffices. P1 direct-only shading is subsumed by the loop.

#include "../common/hit_shading.hlsli"
#include "restir_pack.hlsli"
#include "restir_di.hlsli"

RWTexture2D<float4> g_Output : register(u0);   // rgb = HDR radiance, a = specular hit-distance guide (RR4)
RWTexture2D<float> g_DepthOutput : register(u1); // hyperbolic depth [0,1] at primary hit (DLSS)
RWTexture2D<uint2> g_Metadata : register(u2);  // (instanceId+1, primitiveIndex)
RWTexture2D<float4> g_MotionOutput : register(u3); // NDC motion (curr - prev) at primary hit, matches RT4
// P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md): the same hit that seeds the
// integrator produces every DLSS-RR guide, so color and guides agree at the sub-pixel level.
RWTexture2D<float4> g_DiffuseAlbedoGuide : register(u4);   // RGBA8: albedo·(1−metallic)
RWTexture2D<float4> g_SpecularAlbedoGuide : register(u5);  // RGBA8: EnvBRDFApprox2(F0, roughness², NoV)
RWTexture2D<float4> g_NormalRoughnessGuide : register(u6); // RGBA16F: world normal xyz + roughness w
// G5/R1: first-indirect-vertex sample + M=1 reservoir (passthrough until R2 reuse).
RWStructuredBuffer<RestirGiReservoir> g_GiReservoirCurrent : register(u7);
RWStructuredBuffer<RestirDiReservoirSet> g_ReservoirCurrent : register(u8);
// R2: bounce-0 direct only — temporal shades g_Output = direct + Y·W (never subtract packed Y).
RWTexture2D<float4> g_DirectOutput : register(u9);
RWTexture2D<float4> g_RestirSurfacePositionDepth : register(u10);
RWTexture2D<uint4> g_RestirSurfaceMaterial : register(u11);
RWTexture2D<float4> g_RestirSurfaceAlbedoMetallic : register(u12);
// Independent smooth-dielectric transmission layer for DLSS Ray Reconstruction. The primary
// output and u1/u3/u4-u6 remain the ordinary scene plus the reflection lobe; these resources are
// black/neutral outside supported smooth dielectric primaries.
RWTexture2D<float4> g_OpticalTransmissionOutput : register(u13);
RWTexture2D<float> g_OpticalTransmissionDepth : register(u14);
RWTexture2D<float4> g_OpticalTransmissionMotion : register(u15);
RWTexture2D<float4> g_OpticalTransmissionDiffuseAlbedo : register(u16);
RWTexture2D<float4> g_OpticalTransmissionSpecularAlbedo : register(u17);
RWTexture2D<float4> g_OpticalTransmissionNormalRoughness : register(u18);
RWTexture2D<float4> g_PsrThroughput : register(u19); // rgb=physical delta-prefix throughput, a=owner
RWTexture2D<uint> g_PsrMetadata : register(u20); // length[0:7], terminal[8:11], flags[12:]
RWTexture2D<float2> g_SpecularMotion : register(u21); // dense RR specular motion

// P4b: previous-frame object-to-world rows per compact TLAS InstanceID.
// Explicit rows (row_i = column-major glm m[col][i]) — see DxrPrevInstanceTransformEntry.
struct PrevInstanceTransform
{
    float4 row0;
    float4 row1;
    float4 row2;
};
StructuredBuffer<PrevInstanceTransform> g_PrevInstanceTransforms : register(t14);

// F2/F2b emissive NEE: one entry per emissive instance (S5 step 14 / A1).
struct EmissiveLightEntry
{
    float3 emissive;
    float pickWeight;
    uint instanceId;
    uint triangleOffset;
    uint triangleCount;
    float surfaceArea;
};
StructuredBuffer<EmissiveLightEntry> g_EmissiveLights : register(t15);

struct EmissiveTriangleEntry
{
    float3 v0;
    float pickWeight;
    float3 v1;
    float triangleArea;
    float3 v2;
    float _pad0;
    float3 faceNormal;
    uint primitiveIndex;
};
StructuredBuffer<EmissiveTriangleEntry> g_EmissiveTriangles : register(t18);

// PF3 alias tables preserve the original source weights while selecting in O(1). Triangle aliases
// are stored in each light's existing contiguous triangle range; aliasIndex is absolute.
struct EmissiveAliasEntry
{
    float probability;
    uint aliasIndex;
};
StructuredBuffer<EmissiveAliasEntry> g_EmissiveLightAlias : register(t19);
StructuredBuffer<EmissiveAliasEntry> g_EmissiveTriangleAlias : register(t20);
StructuredBuffer<uint> g_EmissiveLightByInstance : register(t21);

struct PtPsrInstanceBounds
{
    float3 worldBoundsMin;
    uint valid;
    float3 worldBoundsMax;
    uint flags;
};
StructuredBuffer<PtPsrInstanceBounds> g_PtPsrInstanceBounds : register(t22);

// Kept in the PT-only ReSTIR parameter block; this declaration must precede the environment
// helper include because the helper applies it to every HDR lookup.
#define g_EnvironmentRotationY g_PtRestirDiParams.z
#include "pt_env_light.hlsli"
#include "restir_di.hlsli"

// ReSTIR DI initial sampling (roadmap P2): per-category candidate count (0 = off, plain NEE).
#define g_PtRestirDiCandidateCount uint(round(max(g_PtRestirDiParams.x, 0.0)))
#define kPtRestirGiInitialEnabled (g_PtRestirDiParams.y > 0.5)
// Real-time opt-in: split a smooth primary dielectric into its deterministic Fresnel reflection
// and transmission tails.  The host uses the otherwise spare .w component of this PT-only block.
#define kPtDeterministicOpticalSplitEnabled (g_PtRestirDiParams.w > 0.5)

// The shared cbuffer field g_SamplesPerPixel carries the PT max-bounce count (reused verbatim; the
// reflection/GI passes read it as an actual sample count). Alias it for readable PT intent.
#define g_MaxBounces g_SamplesPerPixel

// Path-tracer-only packing in reflection cbuffer fields this pass does not otherwise use.
#define kPtFireflyClampEnabled (g_AoRayCount != 0u)
#define kPtRussianRouletteEnabled (g_HasGiTrace != 0u)
#define kPtCenterPrimaryRays (g_RoughnessCutoff > 0.5)
#define g_PtOpticalStabilityFlagBits uint(round(max(g_PtOpticalStabilityFlags, 0.0)))
#define kPtOpticalMotionReplayEnabled ((g_PtOpticalStabilityFlagBits & 1u) != 0u)
#define kPtIndependentOpticalRrEnabled ((g_PtOpticalStabilityFlagBits & 2u) != 0u)
// Real-time-only mirror ownership. Bit 2 reuses the existing PT-only flag word, so neither the
// root-signature layout nor the g_MaxBounces packing changes.
#define kPtMirrorChainPsrEnabled \
    (((g_PtOpticalStabilityFlagBits & 4u) != 0u) && kPtCenterPrimaryRays)
#define g_PtPsrMaxBounces uint(round(clamp(g_PtPsrParams.x, 1.0, 32.0)))
#define g_PtPsrSubpixelThreshold clamp(g_PtPsrParams.y, 0.0, 2.0)
// The pre-experiment path selected one Fresnel-dominant receiver and used its established inverse
// motion solve. With both lobe-separation features disabled, preserve that path byte-for-byte in
// intent; the replay toggle only controls the experimental separated-lobe route.
#define kPtLegacyOpticalRouting \
    (!kPtDeterministicOpticalSplitEnabled && !kPtIndependentOpticalRrEnabled)
#define kPtUseOpticalMotionReplay \
    (kPtLegacyOpticalRouting || kPtOpticalMotionReplayEnabled)
#define g_PtAmbientStrength g_GiStrength
// Host packs a 0..8 integer as a float (DxrPathTracerDispatch clamps it). saturate() would collapse
// every non-zero setting to a single ray — clamp to the real [0,8] range instead.
#define g_PtAmbientAoRayCount uint(round(clamp(_PadUnjitteredViewProj.x, 0.0, 8.0)))
#define kPtHasInstanceMotion (_PadUnjitteredViewProj.y > 0.5)
// Ray-cone pixel spread angle (radians/pixel ≈ 2·tan(fovY/2)/renderHeight) for albedo texture LOD.
// Mip-0 sampling flickers at texel frequency under DLSS jitter; with P4b the albedo GUIDE comes
// from the PT, and RR remodulates its output with that guide, imprinting the flicker on screen.
#define g_PtPixelSpreadAngle max(_PadUnjitteredViewProj.z, 1e-6)
// Matches lit.vs uTemporalHistoryValid: when false, prevClip = currClip (zero motion).
#define kPtMotionHistoryValid (_PadUnjitteredViewProj.w > 0.5)
// Radiance-term isolation for black-edge debugging (RenderDebugMode PtIsolate*). Host packs modes
// 0..9 as a float; saturate() would collapse every mode >= 2 to DirectSun, making most isolate
// views unreachable — clamp to the real [0,9] range instead.
#ifndef PT_DIAGNOSTIC_PERMUTATION
#define PT_DIAGNOSTIC_PERMUTATION 0
#endif
#ifndef DXR_INLINE_VISIBILITY_PERMUTATION
#define DXR_INLINE_VISIBILITY_PERMUTATION 0
#endif
#ifndef DXR_SER_PERMUTATION
#define DXR_SER_PERMUTATION 0
#endif
#if PT_DIAGNOSTIC_PERMUTATION
#define g_PtDebugIsolateMode uint(round(clamp(_PadPtEmissiveNee, 0.0, 74.0)))
#endif

// Soft sun / ambient AO sample counts (RNG comes from PathRng — no salt blocks, G3).
static const uint kPtSoftSunSampleCount = 4u;
// G7/P5: deep-bounce sun NEE keeps 1 cone sample — RR absorbs the extra variance.
static const uint kPtSoftSunSampleCountDeep = 1u;

// TLAS InstanceMask bits (match DxrAccelerationStructures). TraceRay inclusion = mask & InstanceMask.
static const uint kDxrInstanceMaskOpaque = 0x01u;
static const uint kDxrInstanceMaskDielectric = 0x02u;
static const uint kDxrInstanceMaskAll = 0xFFu;

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagVisibility = 2u;
// G7/P2: request bits OR'd into payload.hit before TraceRay (cleared/replaced on miss/hit).
static const uint kPayloadReqShadingData = 4u; // normal-map, bary, triangleLod
static const uint kPayloadReqPrimarySurface = 8u; // motion + depth (primary only)
static const uint kPayloadHitBackFace = 16u; // packed into hit on closest-hit (G7/P1)
static const uint kPayloadHitMovingInstance = 32u; // primary optical history must omit moving geometry
static const uint kPayloadHitPlanarSurface = 64u; // triangle, vertex normals, and mapped normal agree
static const uint kRussianRouletteStartBounce = 3u;
static const float kRussianRouletteMaxProb = 0.95;
// Below this roughness, specular uses a delta mirror bounce instead of VNDF (alpha floor ~0.032
// otherwise reads as frosted even at roughness 0).
static const float kPtDeltaSpecularRoughness = 0.03;
// Sentinel pdf for delta events (camera ray, perfect mirror, dielectric interface) so MIS gives
// weight ≈ 1 when no BSDF-sampling partner exists (PBRT 14.3.x).
static const float kDeltaScatterPdf = 1.0e10;

// Metadata for the event selected by the actual radiance sample. Producing it consumes no RNG and
// leaves direction, pdf, throughput, and the disabled feature path unchanged.
static const uint kPtScatterEventInvalid = 0u;
static const uint kPtScatterEventDeltaSpecular = 1u;
static const uint kPtScatterEventGlossySpecular = 2u;
static const uint kPtScatterEventDiffuse = 3u;
static const uint kPtScatterEventOptical = 4u;

static const uint kPtPsrTerminalPrimaryReceiver = 0u;
static const uint kPtPsrTerminalReceiver = 1u;
static const uint kPtPsrTerminalEnvironmentEscape = 2u;
static const uint kPtPsrTerminalIneligibleLinkFallback = 3u;
static const uint kPtPsrTerminalSubpixelTail = 4u;
static const uint kPtPsrTerminalNegligibleThroughputTail = 5u;
static const uint kPtPsrTerminalHardCapSignificant = 6u;
static const uint kPtPsrTerminalInvalidProjectionFallback = 7u;

// Payload PACKED to 40 bytes (10 dwords, down from 68/17). MaxPayloadSizeInBytes in DxrPipeline.cpp
// must match. Normals are snorm16 octahedral (uniform ~0.03deg worst-case — precise enough for glass
// refraction, tighter than fp16-oct); barycentrics/lod/prevDepth/motion are fp16 (their consumers —
// UV lookup, a log2 LOD, a relative-depth compare, and an R16G16_FLOAT motion target — are already
// lower precision).
// hitDistance and primaryDepth stay fp32. Access via the Payload* helpers below, never the raw
// fields. Encoding roundtrip is gate-tested in tests/payload_pack_test.cpp.
#if DXR_SER_PERMUTATION
#define PT_RAY_PAYLOAD_ACCESS : read(caller) : write(closesthit, miss)
#define PT_RAY_PAYLOAD [raypayload]
#else
#define PT_RAY_PAYLOAD_ACCESS
#define PT_RAY_PAYLOAD
#endif
struct PT_RAY_PAYLOAD Payload
{
    uint normalOct PT_RAY_PAYLOAD_ACCESS;         // geometric vertex normal (world), snorm16x2 oct
    uint shadingNormalOct PT_RAY_PAYLOAD_ACCESS;  // normal-map perturbed normal (world), snorm16x2 oct
    float hitDistance PT_RAY_PAYLOAD_ACCESS;
    uint instanceId PT_RAY_PAYLOAD_ACCESS;
    uint primitiveIndex PT_RAY_PAYLOAD_ACCESS;
    // Pre-TraceRay: request bits. Post-hit: bit0=1 plus optional back-face, moving-instance, and
    // exact-planar-surface classification bits.
    uint hit : read(caller, closesthit, miss) : write(caller, closesthit, miss);
    uint barycentricsHalf PT_RAY_PAYLOAD_ACCESS;  // barycentrics.xy as fp16 pair
    // low 16: ray-cone albedo LOD constant 0.5*log2(texelArea/worldArea) (RTG ch.20), fp16;
    // high 16: primaryPreviousLinearDepth (GI history validation), fp16.
    uint lodPrevDepthHalf PT_RAY_PAYLOAD_ACCESS;
    // Primary-surface motion from closest-hit: raster-style clip-space interpolation of the hit
    // triangle's vertices (lit.vs currClip/prevClip -> pbr.ps ComputeMotionNdc). fp16 pair; the
    // velocity target is R16G16_FLOAT so this loses nothing.
    uint primaryMotionHalf PT_RAY_PAYLOAD_ACCESS;
    float primaryDepth PT_RAY_PAYLOAD_ACCESS;     // hyperbolic HW depth [0,1] for the DLSS-RR depth guide; keep fp32
};
#undef PT_RAY_PAYLOAD_ACCESS
#undef PT_RAY_PAYLOAD

// PF2: only the primary attributes required to reconnect ReSTIR GI survive the bounce loop.
// Identity, guides, depth, motion, and metadata are committed while processing bounce zero.
struct PtPrimaryReconnectState
{
    bool hit;
    float roughness;
    float dielectricWeight;
    float3 worldPos;
    float3 geomNormal;
    float3 shadingNormal;
    float3 albedo;
    float metallic;
};

// Complete single-domain RR ownership selected by the radiance path. This remains raygen-local;
// no trace payload or cross-language ABI carries it.
struct PtRrGuideOwner
{
    bool valid;
    bool isVirtualReceiver;
    bool terminalSky;
    bool exactDeltaChain;
    float confidence;
    uint chainLength;
    uint instanceId;
    uint primitiveIndex;
    float3 worldPosition;
    float3 shadingNormal;
    float3 diffuseGuide;
    float3 specularGuide;
    float roughness;
    float depth;
    float2 motionNdc;
    float previousDepthDelta;
    float linearDepth;
    float pathDistance;
};

// Camera-to-receiver unfolding for exact static planar mirrors. Appending planes in path order
// composes T = R0 * R1 * ... so applying T to the physical receiver yields the virtual surface
// visible from the camera. Row storage avoids HLSL matrix-layout ambiguity in this raygen-local ABI.
struct PtMirrorVirtualTransform
{
    bool valid;
    float3 row0;
    float3 row1;
    float3 row2;
    float3 translation;
};

PtMirrorVirtualTransform InitPtMirrorVirtualTransform()
{
    PtMirrorVirtualTransform transform;
    transform.valid = true;
    transform.row0 = float3(1.0, 0.0, 0.0);
    transform.row1 = float3(0.0, 1.0, 0.0);
    transform.row2 = float3(0.0, 0.0, 1.0);
    transform.translation = 0.0.xxx;
    return transform;
}

float3 PtMirrorTransformDirection(PtMirrorVirtualTransform transform, float3 direction)
{
    return float3(
        dot(transform.row0, direction),
        dot(transform.row1, direction),
        dot(transform.row2, direction));
}

float3 PtMirrorTransformPoint(PtMirrorVirtualTransform transform, float3 position)
{
    return PtMirrorTransformDirection(transform, position) + transform.translation;
}

bool AppendPtMirrorPlane(
    inout PtMirrorVirtualTransform transform,
    float3 planePoint,
    float3 planeNormal)
{
    const float normalLengthSquared = dot(planeNormal, planeNormal);
    if (!transform.valid || !isfinite(normalLengthSquared) || normalLengthSquared <= 1e-12)
    {
        transform.valid = false;
        return false;
    }

    const float3 normal = planeNormal * rsqrt(normalLengthSquared);
    const float3 transformedNormal = PtMirrorTransformDirection(transform, normal);
    const float planeOffset = dot(normal, planePoint);
    transform.row0 -= 2.0 * transformedNormal.x * normal;
    transform.row1 -= 2.0 * transformedNormal.y * normal;
    transform.row2 -= 2.0 * transformedNormal.z * normal;
    transform.translation += 2.0 * planeOffset * transformedNormal;
    transform.valid = all(isfinite(transform.translation))
        && all(isfinite(transform.row0))
        && all(isfinite(transform.row1))
        && all(isfinite(transform.row2));
    return transform.valid;
}

bool ProjectPtPsrMirrorBounds(
    uint instanceId,
    PtMirrorVirtualTransform precedingMirrors,
    out float projectedSpanPixels)
{
    projectedSpanPixels = 0.0;
    const PtPsrInstanceBounds bounds = g_PtPsrInstanceBounds[instanceId];
    if (bounds.valid == 0u || !precedingMirrors.valid
        || any(bounds.worldBoundsMax < bounds.worldBoundsMin))
    {
        return false;
    }

    float2 minimumNdc = float2(1.0e30, 1.0e30);
    float2 maximumNdc = float2(-1.0e30, -1.0e30);
    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 physicalCorner = float3(
            (corner & 1u) != 0u ? bounds.worldBoundsMax.x : bounds.worldBoundsMin.x,
            (corner & 2u) != 0u ? bounds.worldBoundsMax.y : bounds.worldBoundsMin.y,
            (corner & 4u) != 0u ? bounds.worldBoundsMax.z : bounds.worldBoundsMin.z);
        const float3 virtualCorner = PtMirrorTransformPoint(precedingMirrors, physicalCorner);
        const float4 clip = mul(g_UnjitteredViewProj, float4(virtualCorner, 1.0));
        // Near-plane crossings require homogeneous clipping. Continuing is conservative and avoids
        // a false early stop when the AABB straddles the camera.
        if (!all(isfinite(clip)) || clip.w <= 1.0e-6)
        {
            return false;
        }
        const float2 ndc = clip.xy / clip.w;
        minimumNdc = min(minimumNdc, ndc);
        maximumNdc = max(maximumNdc, ndc);
    }

    if (maximumNdc.x < -1.0 || minimumNdc.x > 1.0
        || maximumNdc.y < -1.0 || minimumNdc.y > 1.0)
    {
        return false;
    }
    const float2 clippedMin = clamp(minimumNdc, -1.0.xx, 1.0.xx);
    const float2 clippedMax = clamp(maximumNdc, -1.0.xx, 1.0.xx);
    const float2 span = max((clippedMax - clippedMin) * 0.5 * float2(g_OutputSize), 0.0.xx);
    projectedSpanPixels = max(span.x, span.y);
    return isfinite(projectedSpanPixels);
}

PtRrGuideOwner InitPtRrGuideOwner()
{
    PtRrGuideOwner owner;
    owner.valid = false;
    owner.isVirtualReceiver = false;
    owner.terminalSky = false;
    owner.exactDeltaChain = false;
    owner.confidence = 0.0;
    owner.chainLength = 0u;
    owner.instanceId = 0u;
    owner.primitiveIndex = 0u;
    owner.worldPosition = 0.0.xxx;
    owner.shadingNormal = float3(0.0, 0.0, 1.0);
    owner.diffuseGuide = 0.5.xxx;
    owner.specularGuide = 0.5.xxx;
    owner.roughness = 1.0;
    owner.depth = 1.0;
    owner.motionNdc = 0.0.xx;
    owner.previousDepthDelta = 0.0;
    owner.linearDepth = g_MaxTraceDistance;
    owner.pathDistance = g_MaxTraceDistance;
    return owner;
}

// snorm16x2 pack/unpack (uniform ~3e-5 precision on [-1,1]; sign-extended unpack).
uint PtPackSnorm16x2(float2 v)
{
    const int2 q = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xffffu) | (uint(q.y) << 16);
}
float2 PtUnpackSnorm16x2(uint p)
{
    const int2 q = int2(int(p << 16) >> 16, int(p) >> 16);
    return clamp(float2(q) * (1.0 / 32767.0), -1.0.xx, 1.0.xx);
}
uint PtPackOctNormal(float3 n) { return PtPackSnorm16x2(RestirOctEncode(n)); }
float3 PtUnpackOctNormal(uint p) { return RestirOctDecode(PtUnpackSnorm16x2(p)); }

// Payload field accessors (packed storage — always read through these).
float3 PayloadGeomNormal(Payload p) { return PtUnpackOctNormal(p.normalOct); }
float3 PayloadShadingNormal(Payload p) { return PtUnpackOctNormal(p.shadingNormalOct); }
float2 PayloadBarycentrics(Payload p) { return RestirUnpackHalf2(p.barycentricsHalf); }
float PayloadTriangleLod(Payload p) { return f16tof32(p.lodPrevDepthHalf & 0xffffu); }
float PayloadPrevLinearDepth(Payload p) { return f16tof32(p.lodPrevDepthHalf >> 16); }
float2 PayloadPrimaryMotion(Payload p) { return RestirUnpackHalf2(p.primaryMotionHalf); }

float2 PixelToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ComputeMotionNdc(float4 currClip, float4 prevClip)
{
    const float2 currNdc = currClip.xy / currClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    return currNdc - prevNdc;
}

float3 PrevWorldFromObject(uint instanceId, float3 objectPos)
{
    const PrevInstanceTransform prev = g_PrevInstanceTransforms[instanceId];
    const float4 p = float4(objectPos, 1.0);
    return float3(dot(prev.row0, p), dot(prev.row1, p), dot(prev.row2, p));
}

// Valid only in closest-hit, where ObjectToWorld3x4 identifies the intersected instance.
bool CurrentInstanceTransformMoved(uint instanceId)
{
    if (!kPtHasInstanceMotion)
    {
        return false;
    }

    const float3x4 current = ObjectToWorld3x4();
    const float3 probeObjectPoints[4] = {
        float3(0.0, 0.0, 0.0), float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };
    [unroll]
    for (uint pointIndex = 0u; pointIndex < 4u; ++pointIndex)
    {
        const float3 objectPoint = probeObjectPoints[pointIndex];
        const float3 currPoint = mul(current, float4(objectPoint, 1.0)).xyz;
        if (length(currPoint - PrevWorldFromObject(instanceId, objectPoint)) > 1e-5)
        {
            return true;
        }
    }
    return false;
}

// DLSS-RR specular albedo guide (vendored ProgrammingGuideDLSS_RR.md §4.2.1, [Ray Tracing Gems
// ch. 32]): preintegrated environment BRDF — NOT raw F0. Canonical call passes alpha = roughness².
// Raw F0 underestimates rough-metal reflectance (missing the scale/bias lift), which over-amplifies
// RR's demodulated specular at metal silhouettes (the 2026-07-11 magenta-fringe episode).
float3 EnvBRDFApprox2(float3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    float4 X;
    X.x = 1.0;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    float4 Y;
    Y.x = 1.0;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    const float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    const float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    const float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    const float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specularColor.g * 50.0); // NVIDIA hack for specular reflectance of 0
    return mad(specularColor, max(0.0, scale), max(0.0, bias));
}

void ResetPayload(inout Payload payload)
{
    payload.normalOct = PtPackOctNormal(float3(0.0, 0.0, 1.0));
    payload.shadingNormalOct = PtPackOctNormal(float3(0.0, 0.0, 1.0));
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload.barycentricsHalf = 0u;
    payload.lodPrevDepthHalf = 0u;
    payload.primaryMotionHalf = 0u;
    payload.primaryDepth = 1.0;
}

bool PayloadIsHit(Payload payload)
{
    return (payload.hit & 1u) != 0u;
}

bool PayloadHitBackFace(Payload payload)
{
    return (payload.hit & kPayloadHitBackFace) != 0u;
}

bool PayloadInstanceMoved(Payload payload)
{
    return (payload.hit & kPayloadHitMovingInstance) != 0u;
}

bool PayloadIsPlanarSurface(Payload payload)
{
    return (payload.hit & kPayloadHitPlanarSurface) != 0u;
}

// Matches lit.vs + pbr.ps: interpolate unjittered curr/prev clip per vertex for MVs; DLSS depth
// uses jittered clip z/w at the same hit (HW-depth convention, motionVectorsJittered = false).
void ComputeVertexInterpolatedPrimarySurface(
    out float2 motionNdc,
    out float primaryDepth,
    out float previousLinearDepth,
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];
    const float w = 1.0 - barycentrics.x - barycentrics.y;
    const float3 bary = float3(w, barycentrics.x, barycentrics.y);

    const float3x4 objectToWorld = ObjectToWorld3x4();
    float4 currClipUnj = 0.0.xxxx;
    float4 prevClipUnj = 0.0.xxxx;
    float4 currClipJit = 0.0.xxxx;

    const uint indices[3] = { i0, i1, i2 };
    [unroll]
    for (uint vi = 0u; vi < 3u; ++vi)
    {
        const float3 objectPos = LoadObjectPosition(geo, indices[vi]);
        const float3 currWorld = mul(objectToWorld, float4(objectPos, 1.0)).xyz;
        float3 prevWorld = currWorld;
        if (kPtHasInstanceMotion)
        {
            prevWorld = PrevWorldFromObject(instanceId, objectPos);
        }

        currClipUnj += mul(g_UnjitteredViewProj, float4(currWorld, 1.0)) * bary[vi];
        prevClipUnj += mul(g_PrevViewProj, float4(prevWorld, 1.0)) * bary[vi];
        currClipJit += mul(g_ViewProj, float4(currWorld, 1.0)) * bary[vi];
    }

    if (!kPtMotionHistoryValid)
    {
        prevClipUnj = currClipUnj;
    }

    motionNdc = ComputeMotionNdc(currClipUnj, prevClipUnj);
    primaryDepth = saturate(currClipJit.z / max(currClipJit.w, 1e-6));
    // Perspective clip W is positive linear view depth in this renderer. Unlike direct comparison
    // of current/previous depths, carrying the expected previous value remains valid under dolly.
    previousLinearDepth = abs(prevClipUnj.w);
}

// GGX VNDF half-vector sampling (Heitz 2018) — same as reflections.hlsl.
float3 SampleGgxVndfHalfVector(float3 normal, float3 viewWorld, float roughness, float2 xi)
{
    const float alpha = max(roughness * roughness, 1e-3);

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    const float3 viewTangent = float3(
        dot(viewWorld, tangent),
        dot(viewWorld, bitangent),
        dot(viewWorld, normal));

    const float3 stretchedView =
        normalize(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));

    const float lenSq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    const float3 basis1 = lenSq > 1e-7
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lenSq)
        : float3(1.0, 0.0, 0.0);
    const float3 basis2 = cross(stretchedView, basis1);

    const float r = sqrt(xi.x);
    const float phi = 2.0 * kPi * xi.y;
    const float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5 * (1.0 + stretchedView.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;

    const float3 halfStretched = t1 * basis1 + t2 * basis2
        + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * stretchedView;

    const float3 halfTangent = normalize(float3(
        alpha * halfStretched.x,
        alpha * halfStretched.y,
        max(halfStretched.z, 1e-6)));

    return normalize(
        tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

#include "pt_dielectric.hlsli"

float TraceVisibilityMasked(float3 origin, float3 direction, float tMax, uint instanceMask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
#if DXR_INLINE_VISIBILITY_PERMUTATION
    // PF6: boolean opaque visibility needs neither a hit group nor the 40-byte surface payload.
    // Keep the flags, mask, and ray interval identical to the legacy TraceRay path below.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(g_SceneTlas, occlusionFlags, instanceMask, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
#else
    Payload probe;
    ResetPayload(probe);
    probe.hit = kPayloadFlagVisibility;
    TraceRay(g_SceneTlas, occlusionFlags, instanceMask, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
#endif
}

// PF7: keep the legacy TraceRay path intact for every non-SER profile. Native HitObject splits
// traversal from closest-hit/miss invocation, allowing the driver to regroup path rays by hit
// coherence. The one-bit continuation hint distinguishes primary-surface setup from continuation
// paths without fragmenting material or texture locality. RNG stays entirely in PathRng/path state.
#if DXR_SER_PERMUTATION
using namespace dx;
#endif
void TracePathRay(inout RayDesc ray, inout Payload payload)
{
#if DXR_SER_PERMUTATION
    const uint continuationHint = (payload.hit & kPayloadReqPrimarySurface) == 0u ? 1u : 0u;
    HitObject hit = HitObject::TraceRay(
        g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);
    // Keep this explicitly scoped: current DXC releases accept the namespace import for
    // HitObject but can emit invalid DXIL for an unqualified MaybeReorderThread call.
    dx::MaybeReorderThread(hit, continuationHint, 1u);
    HitObject::Invoke(hit, payload);
#else
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);
#endif
}

float TraceVisibility(float3 origin, float3 direction, float tMax)
{
    return TraceVisibilityMasked(origin, direction, tMax, kDxrInstanceMaskAll);
}

// Sun NEE with dielectric instance masks (see DxrAccelerationStructures InstanceMask):
//   1) Opaque-only any-hit → hard block (normal shadows stay cheap with glass in the scene)
//   2) Dielectric-only any-hit miss → fully lit (open sun at opaque cost)
//   3) Else TraceTransmissiveVisibility for colored / Fresnel glass shadows
float TraceSunNeeVisibility(float3 origin, float3 direction, float tMax)
{
    if (g_SceneHasTransmission <= 0.5)
    {
        return TraceVisibilityMasked(origin, direction, tMax, kDxrInstanceMaskOpaque);
    }

    if (TraceVisibilityMasked(origin, direction, tMax, kDxrInstanceMaskOpaque) < 0.5)
    {
        return 0.0;
    }

    if (TraceVisibilityMasked(origin, direction, tMax, kDxrInstanceMaskDielectric) > 0.5)
    {
        return 1.0;
    }

    return TraceTransmissiveVisibility(origin, direction, tMax);
}

// Soft sun visibility: cone-jittered shadow rays matching shadows.hlsl / reflections.hlsl.
float TraceSoftSunVisibility(
    float3 origin,
    float3 shadingNormal,
    inout PathRng rng,
    uint sampleCount)
{
    const float3 sunDir = normalize(g_SunDirection);
    if (dot(shadingNormal, sunDir) <= 0.0)
    {
        return 0.0;
    }

    const uint count = max(sampleCount, 1u);
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(sunDir, tangent, bitangent);

    float visSum = 0.0;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
    {
        const float4 xi = PathRngNext4(rng);
        const float diskRadius = sqrt(xi.x);
        const float diskPhi = 2.0 * kPi * xi.y;
        const float2 disk = float2(diskRadius * cos(diskPhi), diskRadius * sin(diskPhi));
        const float3 rayDir = normalize(
            sunDir + (tangent * disk.x + bitangent * disk.y) * g_SunAngularTanRadius);
        visSum += TraceSunNeeVisibility(origin, rayDir, g_MaxTraceDistance);
    }

    return visSum / float(count);
}

// Ray-cone filtered albedo (RTG ch. 20): lod = triangleLod + log2(coneWidth) − log2(n·v).
// Matches the raster path's mip-filtered footprint closely enough that the albedo value is stable
// under sub-pixel jitter — required for the P4b diffuse-albedo GUIDE, which RR remodulates with.
float3 SampleSurfaceAlbedo(uint instanceId, uint primitiveIndex, float2 barycentrics, float albedoLod)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const MaterialEntry material = LoadMaterialForInstance(instanceId);

    float3 albedo = material.albedo;
    if (material.albedoTexIndex != 0xFFFFFFFFu && material.albedoUvOffsetFloats != 0xFFFFFFFFu)
    {
        const float2 hitUv =
            ComputeHitUv(geo, primitiveIndex, material.albedoUvOffsetFloats, barycentrics);
        albedo *= SampleBindlessTextureRgb(material.albedoTexIndex, hitUv, albedoLod);
    }
    return albedo;
}

float ComputeAlbedoLod(Payload payload, float coneWidth, float3 rayDirection)
{
    const float nDotD = max(saturate(dot(PayloadGeomNormal(payload), -rayDirection)), 0.05);
    return PayloadTriangleLod(payload) + log2(max(coneWidth, 1e-6)) - log2(nDotD);
}

// P4b RR material guides — encoding must match full-rr-guides.md / rr_guides.ps.hlsl modes 0–2.
// NVIDIA-canonical guide semantics (vendored ProgrammingGuideDLSS_RR.md §4.2.1), settled by the
// 2026-07-11 guide 2x2 (rr-gi-diagnosis.md §E3): a metal diffuse lean toward neutral 0.5 makes RR
// attribute the metal's noisy specular GI to its diffuse accumulation channel -> intense boil on
// metals; black metal diffuse + RAW-F0 spec under-attributes rough-metal reflectance -> magenta
// silhouette fringe. Canonical black metal diffuse + EnvBRDFApprox2 spec is the remaining quadrant.
void ComputePtPrimaryRrMaterialGuides(
    float3 albedo,
    float3 hitNormal,
    float roughness,
    float metallic,
    float transmission,
    float indexOfRefraction,
    float3 viewDir,
    out float3 diffuseGuide,
    out float3 specGuide,
    out float3 guideNormal,
    out float guideRoughness)
{
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float dielectricWeight = DielectricWeight(transmission, metallic);
    const float nDotV = saturate(dot(hitNormal, viewDir));

    diffuseGuide = albedo * (1.0 - metallic) * (1.0 - dielectricWeight);

    const float dielectricSpec = FresnelDielectric(
        nDotV,
        1.0 / max(indexOfRefraction, 1.0));
    const float3 opaqueSpecGuide = max(
        EnvBRDFApprox2(f0, roughness * roughness, nDotV),
        0.04.xxx);
    specGuide = lerp(
        opaqueSpecGuide,
        float3(dielectricSpec, dielectricSpec, dielectricSpec),
        dielectricWeight);

    guideNormal = normalize(hitNormal);
    guideRoughness = roughness;
}

float ComputeTransmissionGuideAlbedoLod(TransmissionGuideHit guide, float coneWidth)
{
    const float nDotD = max(saturate(dot(guide.normal, -guide.refractDir)), 0.05);
    return guide.triangleLod + log2(max(coneWidth, 1e-6)) - log2(nDotD);
}

float2 ComputeSkyAnchorMotion(float3 anchorDirection)
{
    const float3 skyAnchor = g_CameraPos + anchorDirection * (g_MaxTraceDistance * 0.5);
    float4 currClipUnj = mul(g_UnjitteredViewProj, float4(skyAnchor, 1.0));
    float4 prevClipUnj = mul(g_PrevViewProj, float4(skyAnchor, 1.0));
    if (!kPtMotionHistoryValid)
    {
        prevClipUnj = currClipUnj;
    }
    return ComputeMotionNdc(currClipUnj, prevClipUnj);
}

// Exact static planar chains are one virtual scene: unfold the physical receiver position and normal
// through every accepted mirror plane, then derive depth and motion from that coherent geometry.
// Projecting the physical receiver directly skips the mirror mapping and produces false history.
PtRrGuideOwner BuildPtRrSurfaceGuideOwner(
    Payload payload,
    float3 worldPosition,
    float3 shadingNormal,
    float3 albedo,
    float roughness,
    float metallic,
    float transmission,
    float indexOfRefraction,
    PtMirrorVirtualTransform virtualTransform,
    float3 chainThroughput,
    uint chainLength,
    float pathDistance)
{
    PtRrGuideOwner owner = InitPtRrGuideOwner();
    if (!virtualTransform.valid)
    {
        return owner;
    }

    const float3 virtualPosition = PtMirrorTransformPoint(virtualTransform, worldPosition);
    const float3 virtualNormalUnnormalized =
        PtMirrorTransformDirection(virtualTransform, shadingNormal);
    const float virtualNormalLengthSquared = dot(virtualNormalUnnormalized, virtualNormalUnnormalized);
    const float3 virtualViewVector = g_CameraPos - virtualPosition;
    const float virtualViewLengthSquared = dot(virtualViewVector, virtualViewVector);
    if (!all(isfinite(virtualPosition))
        || !isfinite(virtualNormalLengthSquared) || virtualNormalLengthSquared <= 1e-12
        || !isfinite(virtualViewLengthSquared) || virtualViewLengthSquared <= 1e-12)
    {
        return owner;
    }

    const float3 virtualNormal = virtualNormalUnnormalized * rsqrt(virtualNormalLengthSquared);
    const float3 virtualViewDir = virtualViewVector * rsqrt(virtualViewLengthSquared);
    const float4 currClipJit = mul(g_ViewProj, float4(virtualPosition, 1.0));
    const float4 currClipUnj = mul(g_UnjitteredViewProj, float4(virtualPosition, 1.0));
    float4 prevClipUnj = mul(g_PrevViewProj, float4(virtualPosition, 1.0));
    if (!kPtMotionHistoryValid)
    {
        prevClipUnj = currClipUnj;
    }
    if (!all(isfinite(currClipJit)) || !all(isfinite(currClipUnj))
        || !all(isfinite(prevClipUnj))
        || currClipJit.w <= 1e-6 || currClipUnj.w <= 1e-6 || prevClipUnj.w <= 1e-6)
    {
        return owner;
    }

    owner.valid = true;
    owner.isVirtualReceiver = true;
    owner.exactDeltaChain = true;
    owner.confidence = 1.0;
    owner.chainLength = chainLength;
    owner.instanceId = payload.instanceId;
    owner.primitiveIndex = payload.primitiveIndex;
    owner.worldPosition = virtualPosition;
    owner.shadingNormal = virtualNormal;
    owner.roughness = roughness;
    owner.depth = saturate(currClipJit.z / currClipJit.w);
    owner.motionNdc = ComputeMotionNdc(currClipUnj, prevClipUnj);
    owner.linearDepth = currClipUnj.w;
    owner.previousDepthDelta = prevClipUnj.w - currClipUnj.w;
    owner.pathDistance = pathDistance;
    float3 guideNormal;
    ComputePtPrimaryRrMaterialGuides(
        albedo,
        virtualNormal,
        roughness,
        metallic,
        transmission,
        indexOfRefraction,
        virtualViewDir,
        owner.diffuseGuide,
        owner.specularGuide,
        guideNormal,
        owner.roughness);
    // Receiver material stays in the receiver domain. Delta-prefix attenuation is exported
    // separately through g_PsrThroughput and is never baked into RR material guides.
    if (!all(isfinite(chainThroughput)) || any(chainThroughput < 0.0))
    {
        owner.valid = false;
    }
    owner.shadingNormal = guideNormal;
    return owner;
}

bool ProjectPtPsrVirtualReceiver(
    PtMirrorVirtualTransform virtualTransform,
    float3 physicalWorldPosition,
    out float3 virtualWorldPosition,
    out float virtualDepth,
    out float virtualLinearDepth)
{
    virtualWorldPosition = PtMirrorTransformPoint(virtualTransform, physicalWorldPosition);
    virtualDepth = 1.0;
    virtualLinearDepth = 0.0;
    if (!virtualTransform.valid || !all(isfinite(virtualWorldPosition)))
    {
        return false;
    }

    const float4 clipJittered = mul(g_ViewProj, float4(virtualWorldPosition, 1.0));
    const float4 clipUnjittered = mul(g_UnjitteredViewProj, float4(virtualWorldPosition, 1.0));
    if (!all(isfinite(clipJittered)) || !all(isfinite(clipUnjittered))
        || clipJittered.w <= 1e-6 || clipUnjittered.w <= 1e-6)
    {
        return false;
    }

    virtualDepth = saturate(clipJittered.z / clipJittered.w);
    virtualLinearDepth = clipUnjittered.w;
    return isfinite(virtualDepth) && isfinite(virtualLinearDepth);
}

float2 ComputePtPsrVirtualPointMotion(
    PtMirrorVirtualTransform virtualTransform,
    float3 physicalWorldPosition)
{
    const float3 virtualPosition =
        PtMirrorTransformPoint(virtualTransform, physicalWorldPosition);
    const float4 currentClip = mul(g_UnjitteredViewProj, float4(virtualPosition, 1.0));
    float4 previousClip = mul(g_PrevViewProj, float4(virtualPosition, 1.0));
    if (!kPtMotionHistoryValid)
    {
        previousClip = currentClip;
    }
    if (!virtualTransform.valid || !all(isfinite(currentClip)) || !all(isfinite(previousClip))
        || currentClip.w <= 1e-6 || previousClip.w <= 1e-6)
    {
        return 0.0.xx;
    }
    return ComputeMotionNdc(currentClip, previousClip);
}

PtRrGuideOwner BuildPtRrSkyGuideOwner(
    float3 terminalDirection,
    PtMirrorVirtualTransform virtualTransform,
    float3 chainThroughput,
    uint chainLength,
    float pathDistance)
{
    PtRrGuideOwner owner = InitPtRrGuideOwner();
    const float3 virtualDirectionUnnormalized =
        PtMirrorTransformDirection(virtualTransform, terminalDirection);
    const float virtualDirectionLengthSquared =
        dot(virtualDirectionUnnormalized, virtualDirectionUnnormalized);
    if (!virtualTransform.valid || !isfinite(virtualDirectionLengthSquared)
        || virtualDirectionLengthSquared <= 1e-12)
    {
        return owner;
    }
    const float3 virtualDirection =
        virtualDirectionUnnormalized * rsqrt(virtualDirectionLengthSquared);
    owner.valid = true;
    owner.isVirtualReceiver = true;
    owner.terminalSky = true;
    owner.exactDeltaChain = true;
    owner.confidence = 1.0;
    owner.chainLength = chainLength;
    owner.motionNdc = ComputeSkyAnchorMotion(virtualDirection);
    owner.pathDistance = pathDistance;
    if (!all(isfinite(chainThroughput)) || any(chainThroughput < 0.0))
    {
        owner.valid = false;
    }
    return owner;
}

// A broad glossy lobe is not a single temporal receiver. This confidence is diagnostic-only until
// RR exposes a documented per-pixel validity input. It is continuous in GGX alpha, sampled mixture
// pdf, the world-space cone/lobe footprint, receiver depth, and projected pixel footprint.
float ComputePtGlossyGuideConfidence(
    float roughness,
    float sampledPdf,
    float coneWidthAtReceiver,
    float scatterDistance,
    float receiverLinearDepth)
{
    const float alpha = max(roughness * roughness, 1e-3);
    const float lobeFootprintWorld = max(
        coneWidthAtReceiver,
        alpha * max(scatterDistance, 0.0));
    const float pixelFootprintWorld = max(
        receiverLinearDepth * g_PtPixelSpreadAngle,
        1e-6);
    const float footprintPixels = lobeFootprintWorld / pixelFootprintWorld;
    const float pdfConfidence = sampledPdf / (sampledPdf + 1.0);
    const float slopeConfidence = rcp(1.0 + 32.0 * alpha);
    const float footprintConfidence = rcp(1.0 + footprintPixels * footprintPixels);
    return saturate(pdfConfidence * slopeConfidence * footprintConfidence);
}

void CommitPtRrGuideOwner(
    uint2 pixel,
    PtRrGuideOwner owner,
    inout float specHitDistGuide)
{
    if (!owner.valid || !owner.isVirtualReceiver || !owner.exactDeltaChain)
    {
        return;
    }

    // Logical all-or-nothing bundle. g_Metadata and ReSTIR surface identity intentionally remain
    // primary-surface data; receiver identity is exposed only by the diagnostic permutation.
    g_DepthOutput[pixel] = owner.depth;
    g_MotionOutput[pixel] = float4(owner.motionNdc, owner.previousDepthDelta, 1.0);
    g_DiffuseAlbedoGuide[pixel] = float4(owner.diffuseGuide, 1.0);
    g_SpecularAlbedoGuide[pixel] = float4(owner.specularGuide, 1.0);
    g_NormalRoughnessGuide[pixel] = float4(owner.shadingNormal, owner.roughness);
    g_SpecularMotion[pixel] = owner.motionNdc;
    // The Streamline scalar describes one ray from the primary surface to one hit. A virtualized
    // multi-plane G-buffer has no truthful single segment, so neutralize this optional input rather
    // than feeding the sum of a folded path.
    specHitDistGuide = g_MaxTraceDistance;
}

uint PackPtPsrMetadata(uint chainLength, uint terminalReason, bool exactChain, bool projectionValid)
{
    return min(chainLength, 255u)
        | ((terminalReason & 15u) << 8u)
        | (exactChain ? (1u << 12u) : 0u)
        | (projectionValid ? (1u << 13u) : 0u);
}

void CommitPtPsrPrimaryFallback(
    uint2 pixel,
    Payload payload,
    float3 hitPosition,
    float3 geometricNormal,
    float3 hitNormal,
    float3 albedo,
    float roughness,
    float metallic,
    float transmission,
    float indexOfRefraction,
    float3 viewDir)
{
    float3 diffuseGuide;
    float3 specularGuide;
    float3 guideNormal;
    float guideRoughness;
    ComputePtPrimaryRrMaterialGuides(
        albedo, hitNormal, roughness, metallic, transmission, indexOfRefraction, viewDir,
        diffuseGuide, specularGuide, guideNormal, guideRoughness);
    const float2 primaryMotion = PayloadPrimaryMotion(payload);
    g_DepthOutput[pixel] = payload.primaryDepth;
    g_MotionOutput[pixel] = float4(primaryMotion, 0.0, 1.0);
    g_SpecularMotion[pixel] = primaryMotion;
    g_DiffuseAlbedoGuide[pixel] = float4(diffuseGuide, 1.0);
    g_SpecularAlbedoGuide[pixel] = float4(specularGuide, 1.0);
    g_NormalRoughnessGuide[pixel] = float4(guideNormal, guideRoughness);
    g_Metadata[pixel] = uint2(payload.instanceId + 1u, payload.primitiveIndex);
    const float linearDepth = abs(mul(g_WorldToView, float4(hitPosition, 1.0)).z);
    const uint materialId = g_GeometryLookup[payload.instanceId].materialId;
    const uint surfaceFlags = 1u | (roughness <= kPtDeltaSpecularRoughness ? 4u : 0u);
    g_RestirSurfacePositionDepth[pixel] = float4(hitPosition, linearDepth);
    g_RestirSurfaceMaterial[pixel] = uint4(
        RestirPackOctNormal(geometricNormal),
        RestirPackOctNormal(hitNormal),
        ((payload.instanceId + 1u) & 0x00ffffffu) | (surfaceFlags << 24u),
        (materialId & 0xffffu) | (f32tof16(roughness) << 16u));
    g_RestirSurfaceAlbedoMetallic[pixel] = float4(albedo, metallic);
}

struct PreviousOpticalReceiver
{
    bool valid;
    uint instanceId;
    float3 worldPos;
};

PreviousOpticalReceiver ReplayPreviousOpticalReceiver(
    float2 prevNdc,
    bool transmission)
{
    PreviousOpticalReceiver result;
    result.valid = false;
    result.instanceId = 0u;
    result.worldPos = 0.0.xxx;

    // g_PrevInvViewProj pairs the previous view with the current frame's jittered projection.
    // Every inverse-solve probe uses it so a static camera remains an exactly zero-motion case.
    const float4 prevFarH = mul(g_PrevInvViewProj, float4(prevNdc, 1.0, 1.0));
    const float3 prevRayDir = normalize(prevFarH.xyz / prevFarH.w - g_PrevCameraPos);
    RayDesc prevPrimaryRay;
    prevPrimaryRay.Origin = g_PrevCameraPos;
    prevPrimaryRay.Direction = prevRayDir;
    prevPrimaryRay.TMin = 0.001;
    prevPrimaryRay.TMax = g_MaxTraceDistance;

    Payload prevPrimary;
    ResetPayload(prevPrimary);
    prevPrimary.hit = kPayloadReqShadingData | kPayloadReqPrimarySurface;
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, prevPrimaryRay, prevPrimary);
    if (prevPrimary.hit == 0u)
    {
        return result;
    }

    const float3 prevPrimaryPos = g_PrevCameraPos + prevRayDir * prevPrimary.hitDistance;
    const float3 prevGeomNormal = PayloadGeomNormal(prevPrimary);
    const float prevNdotV = saturate(dot(prevGeomNormal, -prevRayDir));
    const float prevOriginBias =
        max(prevPrimary.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - prevNdotV));

    if (transmission)
    {
        const MaterialEntry prevMaterial = LoadMaterialForInstance(prevPrimary.instanceId);
        if (DielectricWeight(prevMaterial.transmission, prevMaterial.metallic) <= 0.0)
        {
            return result;
        }
        const bool prevThin = prevMaterial.thinWalled > 0.5;
        const FirstOpticalInterface prevOptical = BuildFirstOpticalInterface(
            PayloadShadingNormal(prevPrimary),
            prevRayDir,
            prevMaterial.indexOfRefraction,
            prevThin,
            false);
        const TransmissionGuideHit guide = TraceTransmissionGuide(
            prevPrimaryPos,
            prevOptical,
            prevMaterial.indexOfRefraction,
            prevThin ? max(prevOriginBias, kThinShellMinExitBias) : prevOriginBias,
            prevPrimary.instanceId);
        result.valid = guide.valid;
        result.instanceId = guide.instanceId;
        result.worldPos = guide.backgroundWorldPos;
    }
    else
    {
        const ReflectionGuideHit guide = TraceReflectionGuide(
            prevPrimaryPos,
            normalize(reflect(prevRayDir, PayloadShadingNormal(prevPrimary))),
            prevOriginBias);
        result.valid = guide.valid;
        result.instanceId = guide.instanceId;
        result.worldPos = guide.receiverWorldPos;
    }
    return result;
}

bool MatchesOpticalReceiver(
    PreviousOpticalReceiver candidate,
    uint receiverInstanceId)
{
    return candidate.valid && candidate.instanceId == receiverInstanceId;
}

// Invert F(previousNdc) = receiverWorldPos.  The primary-surface seed is exact for pure camera
// rotation; the direct receiver seed is exact for a thin straight-through pane.  We replay both,
// choose the closer physical path, then refine it with a 3D least-squares Newton step.
float2 SolvePreviousOpticalReceiverMotion(
    uint2 pixel,
    float3 receiverWorldPos,
    uint receiverInstanceId,
    bool transmission,
    float2 primarySurfaceMotion,
    out bool replayValid)
{
    const float2 currNdc = PixelToClipXY((float2(pixel) + 0.5) / float2(g_OutputSize));
    const float4 directPrevClip = mul(g_PrevViewProj, float4(receiverWorldPos, 1.0));
    const float2 directPrevNdc = directPrevClip.xy / max(directPrevClip.w, 1e-6);
    const float2 primaryPrevNdc = currNdc - primarySurfaceMotion;

    const PreviousOpticalReceiver directReplay = ReplayPreviousOpticalReceiver(
        directPrevNdc, transmission);
    const PreviousOpticalReceiver primaryReplay = ReplayPreviousOpticalReceiver(
        primaryPrevNdc, transmission);
    const bool directMatches = MatchesOpticalReceiver(directReplay, receiverInstanceId);
    const bool primaryMatches = MatchesOpticalReceiver(primaryReplay, receiverInstanceId);
    const float directError = directMatches
        ? dot(directReplay.worldPos - receiverWorldPos, directReplay.worldPos - receiverWorldPos)
        : 1.0e30;
    const float primaryError = primaryMatches
        ? dot(primaryReplay.worldPos - receiverWorldPos, primaryReplay.worldPos - receiverWorldPos)
        : 1.0e30;

    float2 previousNdc = directPrevNdc;
    PreviousOpticalReceiver center = directReplay;
    if (primaryError < directError)
    {
        previousNdc = primaryPrevNdc;
        center = primaryReplay;
    }
    replayValid = directMatches || primaryMatches;
    if (!replayValid)
    {
        // The receiver is absent from both prior optical paths: this is a disocclusion, not a
        // conservative optical gate.  RR's ordinary depth/identity validation owns this case.
        return currNdc - directPrevNdc;
    }

    const float2 probeStep = 3.0 / float2(g_OutputSize);
    [unroll]
    for (uint iteration = 0u; iteration < 2u; ++iteration)
    {
        const PreviousOpticalReceiver probeX = ReplayPreviousOpticalReceiver(
            previousNdc + float2(probeStep.x, 0.0), transmission);
        const PreviousOpticalReceiver probeY = ReplayPreviousOpticalReceiver(
            previousNdc + float2(0.0, probeStep.y), transmission);
        if (!MatchesOpticalReceiver(probeX, receiverInstanceId)
            || !MatchesOpticalReceiver(probeY, receiverInstanceId))
        {
            break;
        }

        const float3 dFdx = (probeX.worldPos - center.worldPos) / probeStep.x;
        const float3 dFdy = (probeY.worldPos - center.worldPos) / probeStep.y;
        const float3 residual = center.worldPos - receiverWorldPos;
        const float a = dot(dFdx, dFdx);
        const float b = dot(dFdx, dFdy);
        const float c = dot(dFdy, dFdy);
        const float determinant = a * c - b * b;
        if (determinant <= max((a + c) * (a + c) * 1.0e-8, 1.0e-12))
        {
            break;
        }

        float2 step;
        step.x = (-c * dot(dFdx, residual) + b * dot(dFdy, residual)) / determinant;
        step.y = ( b * dot(dFdx, residual) - a * dot(dFdy, residual)) / determinant;
        // A Newton trust region prevents a discontinuous finite-difference probe from jumping
        // across most of the screen before the next exact optical replay; it does not reject
        // history or replace the solved coordinate with a fallback.
        const float stepLength = length(step);
        if (stepLength > 0.25)
        {
            step *= 0.25 / stepLength;
        }
        previousNdc += step;
        center = ReplayPreviousOpticalReceiver(previousNdc, transmission);
        if (!MatchesOpticalReceiver(center, receiverInstanceId))
        {
            replayValid = false;
            break;
        }
    }

    return currNdc - previousNdc;
}

float2 ComputeTransmissionVirtualMotion(
    uint2 pixel,
    TransmissionGuideHit currGuide,
    float2 primarySurfaceMotion,
    out bool replayValid)
{
    replayValid = false;
    if (!kPtMotionHistoryValid)
    {
        replayValid = currGuide.valid;
        return currGuide.valid ? currGuide.motion : 0.0.xx;
    }
    if (!currGuide.valid)
    {
        return ComputeSkyAnchorMotion(currGuide.refractDir);
    }
    if (!kPtUseOpticalMotionReplay)
    {
        replayValid = true;
        return currGuide.motion;
    }

    return SolvePreviousOpticalReceiverMotion(
        pixel,
        currGuide.backgroundWorldPos,
        currGuide.instanceId,
        true,
        primarySurfaceMotion,
        replayValid);
}

struct PreviousReflectionSky
{
    bool valid;
    float3 reflectDir;
};

PreviousReflectionSky ReplayPreviousReflectionSky(float2 prevNdc)
{
    PreviousReflectionSky result;
    result.valid = false;
    result.reflectDir = 0.0.xxx;

    const float4 prevFarH = mul(g_PrevInvViewProj, float4(prevNdc, 1.0, 1.0));
    const float3 prevRayDir = normalize(prevFarH.xyz / prevFarH.w - g_PrevCameraPos);
    RayDesc prevPrimaryRay;
    prevPrimaryRay.Origin = g_PrevCameraPos;
    prevPrimaryRay.Direction = prevRayDir;
    prevPrimaryRay.TMin = 0.001;
    prevPrimaryRay.TMax = g_MaxTraceDistance;

    Payload prevPrimary;
    ResetPayload(prevPrimary);
    prevPrimary.hit = kPayloadReqShadingData | kPayloadReqPrimarySurface;
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, prevPrimaryRay, prevPrimary);
    if (prevPrimary.hit == 0u)
    {
        return result;
    }

    const MaterialEntry prevMaterial = LoadMaterialForInstance(prevPrimary.instanceId);
    const float prevDielectricWeight =
        DielectricWeight(prevMaterial.transmission, prevMaterial.metallic);
    if (prevDielectricWeight <= 0.0 && prevMaterial.metallic < 0.5)
    {
        return result;
    }

    const float3 prevPrimaryPos = g_PrevCameraPos + prevRayDir * prevPrimary.hitDistance;
    const float3 prevGeomNormal = PayloadGeomNormal(prevPrimary);
    const float prevNdotV = saturate(dot(prevGeomNormal, -prevRayDir));
    const float prevOriginBias =
        max(prevPrimary.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - prevNdotV));
    const float3 reflectDir = normalize(reflect(prevRayDir, PayloadShadingNormal(prevPrimary)));
    const ReflectionGuideHit guide =
        TraceReflectionGuide(prevPrimaryPos, reflectDir, prevOriginBias);
    if (guide.valid)
    {
        return result;
    }

    result.valid = true;
    result.reflectDir = reflectDir;
    return result;
}

float ReflectionSkyDirectionError(PreviousReflectionSky replay, float3 targetDirection)
{
    return replay.valid ? 1.0 - saturate(dot(replay.reflectDir, targetDirection)) : 1.0e30;
}

float2 SolvePreviousReflectionSkyMotion(
    uint2 pixel,
    float3 targetDirection,
    float2 primarySurfaceMotion,
    out bool replayValid)
{
    const float2 currNdc = PixelToClipXY((float2(pixel) + 0.5) / float2(g_OutputSize));
    const float2 skySeed = currNdc - ComputeSkyAnchorMotion(targetDirection);
    const float2 primarySeed = currNdc - primarySurfaceMotion;
    const PreviousReflectionSky skyReplay = ReplayPreviousReflectionSky(skySeed);
    const PreviousReflectionSky primaryReplay = ReplayPreviousReflectionSky(primarySeed);

    float2 previousNdc = skySeed;
    PreviousReflectionSky center = skyReplay;
    if (ReflectionSkyDirectionError(primaryReplay, targetDirection)
        < ReflectionSkyDirectionError(skyReplay, targetDirection))
    {
        previousNdc = primarySeed;
        center = primaryReplay;
    }
    replayValid = center.valid;
    if (!replayValid)
    {
        return currNdc - skySeed;
    }

    const float2 probeStep = 3.0 / float2(g_OutputSize);
    [unroll]
    for (uint iteration = 0u; iteration < 2u; ++iteration)
    {
        const PreviousReflectionSky probeX = ReplayPreviousReflectionSky(
            previousNdc + float2(probeStep.x, 0.0));
        const PreviousReflectionSky probeY = ReplayPreviousReflectionSky(
            previousNdc + float2(0.0, probeStep.y));
        if (!probeX.valid || !probeY.valid)
        {
            break;
        }

        const float3 dFdx = (probeX.reflectDir - center.reflectDir) / probeStep.x;
        const float3 dFdy = (probeY.reflectDir - center.reflectDir) / probeStep.y;
        const float3 residual = center.reflectDir - targetDirection;
        const float a = dot(dFdx, dFdx);
        const float b = dot(dFdx, dFdy);
        const float c = dot(dFdy, dFdy);
        const float determinant = a * c - b * b;
        if (determinant <= max((a + c) * (a + c) * 1.0e-8, 1.0e-12))
        {
            break;
        }

        float2 step;
        step.x = (-c * dot(dFdx, residual) + b * dot(dFdy, residual)) / determinant;
        step.y = ( b * dot(dFdx, residual) - a * dot(dFdy, residual)) / determinant;
        const float stepLength = length(step);
        if (stepLength > 0.25)
        {
            step *= 0.25 / stepLength;
        }
        previousNdc += step;
        center = ReplayPreviousReflectionSky(previousNdc);
        if (!center.valid)
        {
            replayValid = false;
            break;
        }
    }

    return currNdc - previousNdc;
}

float2 ComputeReflectionVirtualMotion(
    uint2 pixel,
    ReflectionGuideHit currGuide,
    float2 primarySurfaceMotion,
    out bool replayValid)
{
    replayValid = false;
    if (!currGuide.valid)
    {
        if (kPtLegacyOpticalRouting)
        {
            return 0.0.xx;
        }
        if (!kPtMotionHistoryValid)
        {
            replayValid = true;
            return 0.0.xx;
        }
        if (!kPtOpticalMotionReplayEnabled)
        {
            replayValid = true;
            return ComputeSkyAnchorMotion(currGuide.reflectDir);
        }
        return SolvePreviousReflectionSkyMotion(
            pixel, currGuide.reflectDir, primarySurfaceMotion, replayValid);
    }
    if (!kPtMotionHistoryValid)
    {
        replayValid = true;
        return currGuide.motion;
    }
    if (!kPtUseOpticalMotionReplay)
    {
        replayValid = true;
        return currGuide.motion;
    }

    return SolvePreviousOpticalReceiverMotion(
        pixel,
        currGuide.receiverWorldPos,
        currGuide.instanceId,
        false,
        primarySurfaceMotion,
        replayValid);
}

// Static PSR glass uses the same affine unfolded receiver point for current/previous projection.
// Replaying mirror links inside every optical Newton probe inflated the diagnostic DXIL by ~100 KB
// and made Debug RTPSO creation exceed seven minutes; a dedicated prepass is the appropriate future
// home for that nonlinear refinement. This projection remains coherent with the exported virtual
// depth/normal and is exact for a static camera.
float2 ComputePsrOpticalReceiverMotion(
    float3 physicalReceiverPosition,
    PtMirrorVirtualTransform mirrorVirtualTransform,
    out bool replayValid)
{
    replayValid = false;
    if (!kPtMotionHistoryValid)
    {
        replayValid = true;
        return 0.0.xx;
    }
    replayValid = true;
    return ComputePtPsrVirtualPointMotion(
        mirrorVirtualTransform, physicalReceiverPosition);
}

void ComputeOpticalReceiverMaterialGuides(
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics,
    float3 receiverGeomNormal,
    float3 receiverShadingNormal,
    float3 receiverDir,
    float triangleLod,
    float coneWidth,
    out float3 diffuseGuide,
    out float3 specGuide,
    out float3 guideNormal,
    out float guideRoughness)
{
    const MaterialEntry receiverMaterial = LoadMaterialForInstance(instanceId);
    const float nDotD = max(saturate(dot(receiverGeomNormal, -receiverDir)), 0.05);
    const float receiverLod = triangleLod + log2(max(coneWidth, 1e-6)) - log2(nDotD);
    float3 receiverAlbedo;
    float receiverMetallic;
    float3 receiverEmissive;
    ResolveSurfaceMaterialScalars(
        instanceId,
        primitiveIndex,
        barycentrics,
        receiverLod,
        receiverAlbedo,
        guideRoughness,
        receiverMetallic,
        receiverEmissive);
    ComputePtPrimaryRrMaterialGuides(
        receiverAlbedo,
        receiverShadingNormal,
        guideRoughness,
        receiverMetallic,
        receiverMaterial.transmission,
        receiverMaterial.indexOfRefraction,
        -receiverDir,
        diffuseGuide,
        specGuide,
        guideNormal,
        guideRoughness);
}

#if PT_DIAGNOSTIC_PERMUTATION
// Replays the PREVIOUS optical path at the receiver location selected by the exported motion.
// Output is (normalized world-position error, same receiver instance, valid previous trace).
// Unlike the old same-pixel replay, this directly tests the history coordinate RR will sample.
float3 DiagnoseTransmissionReceiverReprojection(
    uint2 pixel,
    float2 opticalMotion,
    TransmissionGuideHit currGuide,
    float originBias)
{
    if (!kPtMotionHistoryValid || !currGuide.valid)
    {
        return 0.0.xxx;
    }

    const float2 currNdc = PixelToClipXY((float2(pixel) + 0.5) / float2(g_OutputSize));
    const float2 prevNdc = currNdc - opticalMotion;
    const float4 prevFarH = mul(g_PrevInvViewProj, float4(prevNdc, 1.0, 1.0));
    const float3 prevRayDir = normalize(prevFarH.xyz / prevFarH.w - g_PrevCameraPos);
    RayDesc prevPrimaryRay;
    prevPrimaryRay.Origin = g_PrevCameraPos;
    prevPrimaryRay.Direction = prevRayDir;
    prevPrimaryRay.TMin = 0.001;
    prevPrimaryRay.TMax = g_MaxTraceDistance;
    Payload prevPrimary;
    ResetPayload(prevPrimary);
    prevPrimary.hit = kPayloadReqShadingData | kPayloadReqPrimarySurface;
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, prevPrimaryRay, prevPrimary);
    if (prevPrimary.hit == 0)
    {
        return float3(1.0, 0.0, 0.0);
    }

    const MaterialEntry prevMaterial = LoadMaterialForInstance(prevPrimary.instanceId);
    if (DielectricWeight(prevMaterial.transmission, prevMaterial.metallic) <= 0.0)
    {
        return float3(1.0, 0.0, 0.0);
    }
    const bool prevThin = prevMaterial.thinWalled > 0.5;
    const FirstOpticalInterface prevOptical = BuildFirstOpticalInterface(
        PayloadShadingNormal(prevPrimary), prevRayDir, prevMaterial.indexOfRefraction, prevThin, false);
    const float3 prevPrimaryPos = g_PrevCameraPos + prevRayDir * prevPrimary.hitDistance;
    const TransmissionGuideHit prevGuide = TraceTransmissionGuide(
        prevPrimaryPos,
        prevOptical,
        prevMaterial.indexOfRefraction,
        prevThin ? max(originBias, kThinShellMinExitBias) : originBias,
        prevPrimary.instanceId);
    if (!prevGuide.valid)
    {
        return float3(1.0, 0.0, 0.0);
    }

    const float positionError = length(prevGuide.backgroundWorldPos - currGuide.backgroundWorldPos)
        / max(currGuide.refractedHitDistance, 0.05);
    return float3(saturate(positionError * 8.0),
        prevGuide.instanceId == currGuide.instanceId ? 1.0 : 0.0, 1.0);
}

float3 DiagnoseReflectionReceiverReprojection(
    uint2 pixel,
    float2 opticalMotion,
    ReflectionGuideHit currGuide,
    float originBias)
{
    if (!kPtMotionHistoryValid)
    {
        return 0.0.xxx;
    }

    const float2 currNdc = PixelToClipXY((float2(pixel) + 0.5) / float2(g_OutputSize));
    const float2 prevNdc = currNdc - opticalMotion;
    if (!currGuide.valid)
    {
        const PreviousReflectionSky previousSky = ReplayPreviousReflectionSky(prevNdc);
        const float directionError = previousSky.valid
            ? 1.0 - saturate(dot(previousSky.reflectDir, currGuide.reflectDir))
            : 1.0;
        return float3(saturate(directionError * 1024.0),
            previousSky.valid ? 1.0 : 0.0, previousSky.valid ? 1.0 : 0.0);
    }

    const float4 prevFarH = mul(g_PrevInvViewProj, float4(prevNdc, 1.0, 1.0));
    const float3 prevRayDir = normalize(prevFarH.xyz / prevFarH.w - g_PrevCameraPos);
    RayDesc prevPrimaryRay;
    prevPrimaryRay.Origin = g_PrevCameraPos;
    prevPrimaryRay.Direction = prevRayDir;
    prevPrimaryRay.TMin = 0.001;
    prevPrimaryRay.TMax = g_MaxTraceDistance;
    Payload prevPrimary;
    ResetPayload(prevPrimary);
    prevPrimary.hit = kPayloadReqShadingData | kPayloadReqPrimarySurface;
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, prevPrimaryRay, prevPrimary);
    if (prevPrimary.hit == 0)
    {
        return float3(1.0, 0.0, 0.0);
    }

    const float3 prevPrimaryPos = g_PrevCameraPos + prevRayDir * prevPrimary.hitDistance;
    const ReflectionGuideHit prevGuide = TraceReflectionGuide(
        prevPrimaryPos,
        normalize(reflect(prevRayDir, PayloadShadingNormal(prevPrimary))),
        originBias);
    if (!prevGuide.valid)
    {
        return float3(1.0, 0.0, 0.0);
    }

    const float positionError = length(prevGuide.receiverWorldPos - currGuide.receiverWorldPos)
        / max(currGuide.reflectedHitDistance, 0.05);
    return float3(saturate(positionError * 8.0),
        prevGuide.instanceId == currGuide.instanceId ? 1.0 : 0.0, 1.0);
}

float3 EncodeOpticalReplayStatus(
    bool currentGuideValid,
    bool replayValid,
    float3 reprojection)
{
    if (!currentGuideValid)
    {
        return 0.0.xxx;
    }
    if (!kPtMotionHistoryValid)
    {
        return float3(0.0, 0.0, 1.0);
    }
    if (!replayValid || reprojection.z < 0.5)
    {
        return float3(1.0, 0.0, 0.0);
    }
    if (reprojection.y < 0.5)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (reprojection.x > 0.1)
    {
        return float3(1.0, 1.0, 0.0);
    }
    return float3(0.0, 1.0, 0.0);
}
#endif

float TracePrimaryAmbientOcclusion(inout PathRng rng, float3 origin, float3 normal, uint rayCount)
{
    if (rayCount == 0u)
    {
        return 1.0;
    }

    const float aoRadius = max(g_MaxTraceDistance * 0.05, 0.5);
    float aoSum = 0.0;
    [loop]
    for (uint aoIndex = 0u; aoIndex < rayCount; ++aoIndex)
    {
        const float2 aoXi = PathRngNext2(rng);
        const float3 aoDir = CosineSampleHemisphere(normal, aoXi);
        aoSum += TraceVisibility(origin, aoDir, aoRadius);
    }

    return aoSum / float(rayCount);
}

float3 EvaluateRealTimeDiffuseAmbient(
    float3 diffuseAlbedo,
    float3 shadingNormal,
    float aoVisibility)
{
    // Environment IS NEE replaces the SH ambient floor when the CDF is bound (F2).
    if (g_EnvLightImportanceCount > 0u)
    {
        return 0.0.xxx;
    }

    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    if (diffuseWeight <= 0.02)
    {
        return 0.0.xxx;
    }

    const float3 irradiance = EvaluateDiffuseIrradianceSh(shadingNormal);
    return diffuseAlbedo * irradiance / kPi
        * aoVisibility
        * g_EnvironmentIntensity
        * g_PtAmbientStrength;
}

float3 EvaluateRealTimeEmitterDisplayAmbient(
    float3 diffuseAlbedo,
    float3 shadingNormal)
{
    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    if (diffuseWeight <= 0.02)
    {
        return 0.0.xxx;
    }

    // Camera-visible emissive meshes should not become flat full-bright cards in real-time view.
    // Use the emissive material for transport, but keep the primary surface display shaded without
    // spending AO or secondary rays on the emitter terminal.
    const float3 irradiance = EvaluateDiffuseIrradianceSh(shadingNormal);
    return diffuseAlbedo * irradiance / kPi
        * g_EnvironmentIntensity
        * g_PtAmbientStrength;
}

float TracePrimarySunVisibility(
    inout PathRng rng,
    float3 hitNormal,
    float3 shadowOrigin,
    uint softSunSampleCount)
{
    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    if (ndotl <= 0.0)
    {
        return 0.0;
    }

    return (g_SunAngularTanRadius > 1e-6)
        ? TraceSoftSunVisibility(shadowOrigin, hitNormal, rng, softSunSampleCount)
        : TraceSunNeeVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
}

float BalanceHeuristic(float pdfA, float pdfB)
{
    const float denom = pdfA + pdfB;
    return denom > 1e-6 ? pdfA / denom : 1.0;
}

uint SampleEmissiveAlias(
    StructuredBuffer<EmissiveAliasEntry> aliases,
    uint offset,
    uint count,
    float xi)
{
    const float scaled = saturate(xi) * float(count);
    const uint bucket = min(uint(scaled), count - 1u);
    const EmissiveAliasEntry entry = aliases[offset + bucket];
    return frac(scaled) < entry.probability ? offset + bucket : entry.aliasIndex;
}

uint EmissiveLightIndexFromInstance(uint instanceId)
{
    uint instanceCount;
    uint instanceStride;
    g_EmissiveLightByInstance.GetDimensions(instanceCount, instanceStride);
    if (instanceId >= instanceCount)
    {
        return 0xffffffffu;
    }
    return g_EmissiveLightByInstance[instanceId];
}

float EmissiveLightPickPdf(uint instanceId)
{
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return 0.0;
    }

    const uint lightIndex = EmissiveLightIndexFromInstance(instanceId);
    return lightIndex != 0xffffffffu
        ? g_EmissiveLights[lightIndex].pickWeight / g_EmissiveLightPickWeightSum
        : 0.0;
}

// Look up an emitter's NEE selection pdf AND its total mesh surface area by instance id, so a
// BSDF-sampled ray that lands on an emitter can compute the competing NEE density for MIS.
void EmissiveLightLookup(uint instanceId, out float pickPdf, out float surfaceArea)
{
    pickPdf = 0.0;
    surfaceArea = 0.0;
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return;
    }

    const uint lightIndex = EmissiveLightIndexFromInstance(instanceId);
    if (lightIndex != 0xffffffffu)
    {
        const EmissiveLightEntry light = g_EmissiveLights[lightIndex];
        pickPdf = light.pickWeight / g_EmissiveLightPickWeightSum;
        surfaceArea = light.surfaceArea;
    }
}

// Solid-angle density of the emissive-NEE strategy for a light seen at `dist` with emitter-facing
// cosine `cosThetaEmitter`: pickPdf * (1/area) * dist^2 / cos. Instance-level pickPdf + area are
// used for the BSDF-hit MIS partner (triangle NEE uses per-triangle area on the light side).
float EmissiveNeePdfSolidAngle(float pickPdf, float surfaceArea, float dist2, float cosThetaEmitter)
{
    if (pickPdf <= 0.0 || surfaceArea <= 1e-8 || cosThetaEmitter <= 1e-6)
    {
        return 0.0;
    }
    return pickPdf * (1.0 / surfaceArea) * dist2 / cosThetaEmitter;
}

// Uniform sample on a triangle; pdfArea is 1 / triangle area.
void SampleUniformPointOnTriangle(
    float3 v0,
    float3 v1,
    float3 v2,
    float triangleArea,
    float2 xi,
    out float3 surfacePoint,
    out float pdfArea)
{
    if (triangleArea <= 1e-8)
    {
        surfacePoint = (v0 + v1 + v2) / 3.0;
        pdfArea = 1.0;
        return;
    }

    const float sqrtXi0 = sqrt(saturate(xi.x));
    const float u = 1.0 - sqrtXi0;
    const float v = xi.y * sqrtXi0;
    const float w = 1.0 - u - v;
    surfacePoint = u * v0 + v * v1 + w * v2;
    pdfArea = 1.0 / triangleArea;
}

// GGX/Trowbridge-Reitz + height-correlated Smith helpers for the opaque BRDF estimator.
// alpha MUST match SampleGgxVndfHalfVector's internal floor (1e-3) so the evaluated pdf corresponds
// to the sampling distribution (a mismatch biases the estimator).
float GgxD(float NoH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-9);
}

float SmithG1(float NoX, float alpha)
{
    const float a2 = alpha * alpha;
    return 2.0 * NoX / max(NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX), 1e-9);
}

float SmithG2HeightCorrelated(float NoV, float NoL, float alpha)
{
    const float a2 = alpha * alpha;
    const float lambdaV = NoL * sqrt(a2 + (1.0 - a2) * NoV * NoV);
    const float lambdaL = NoV * sqrt(a2 + (1.0 - a2) * NoL * NoL);
    return 2.0 * NoV * NoL / max(lambdaV + lambdaL, 1e-9);
}

// Plain Schlick Fresnel (no roughness ceiling — that raster-IBL hack, R4, does not belong in the PT
// energy split). f0 = 0.04 dielectric / albedo metal.
float3 FresnelSchlick(float cosTheta, float3 f0)
{
    const float m = saturate(1.0 - cosTheta);
    const float m2 = m * m;
    return f0 + (1.0.xxx - f0) * (m2 * m2 * m);
}

bool OpaqueHasNoDiffuseLobe(float3 albedo, float metallic)
{
    const float3 baseDiffuse = abs(albedo * (1.0 - saturate(metallic)));
    return max(baseDiffuse.r, max(baseDiffuse.g, baseDiffuse.b)) <= 1e-6;
}

bool IsPtOpaqueDeltaOnly(
    float3 albedo,
    float roughness,
    float metallic,
    float dielectricWeight)
{
    return dielectricWeight <= 0.0
        && roughness <= kPtDeltaSpecularRoughness
        && OpaqueHasNoDiffuseLobe(albedo, metallic);
}

bool IsPtExactPlanarDeltaMirror(
    Payload payload,
    float3 albedo,
    float roughness,
    float metallic,
    float dielectricWeight)
{
    return IsPtOpaqueDeltaOnly(albedo, roughness, metallic, dielectricWeight)
        && PayloadIsPlanarSurface(payload)
        && !PayloadInstanceMoved(payload);
}

float OpaqueBsdfLobeSelectionProbFromNoV(float NoV, float3 f0, float3 albedo, float metallic)
{
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);
    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));
    if (OpaqueHasNoDiffuseLobe(albedo, metallic))
    {
        // A zero-energy diffuse lobe is not a useful sampling technique. The former 0.9 ceiling
        // generated one near-black path per ten perfect-metal bounces, compounding through chains.
        return 1.0;
    }
    const float specLum = Luminance(fresnelNoV);
    const float diffLum = Luminance(baseDiffuse);
    float pSpec = specLum / max(specLum + diffLum, 1e-4);
    pSpec = lerp(pSpec, 1.0, saturate(metallic));
    return clamp(pSpec, 0.1, 0.9);
}

// Reject only when the light lies behind the tangent plane. Do not early-out on grazing view
// (NoV → 0): raster uses a denominator floor (pbr.ps.hlsl CalcCookTorranceContribution) so
// silhouettes stay lit when the sun is visible — a hard NoV cutoff drew black void rims.
float3 EvaluateOpaqueBsdf(
    float3 hitNormal,
    float3 viewDir,
    float3 wi,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3);
    const float NoV = saturate(dot(hitNormal, viewDir));
    const float NoL = saturate(dot(hitNormal, wi));
    if (NoL <= 0.0)
    {
        return 0.0.xxx;
    }

    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);
    const float3 h = normalize(viewDir + wi);
    const float NoH = saturate(dot(hitNormal, h));
    const float VoH = saturate(dot(viewDir, h));

    const float d = GgxD(NoH, alpha);
    const float g2 = SmithG2HeightCorrelated(NoV, NoL, alpha);
    const float3 fresnel = FresnelSchlick(VoH, f0);

    const float3 specCos = d * g2 * fresnel / max(4.0 * NoV, 1e-4);
    const float3 diffCos = baseDiffuse * (1.0.xxx - fresnelNoV) * (NoL / kPi);
    return specCos + diffCos;
}

// Mixture directional pdf for sampling direction wi (matches SampleOpaqueInterface's pdfMix).
float OpaqueBsdfPdf(
    float3 hitNormal,
    float3 viewDir,
    float3 wi,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3);
    const float NoV = saturate(dot(hitNormal, viewDir));
    const float NoL = saturate(dot(hitNormal, wi));
    if (NoL <= 0.0)
    {
        return 0.0;
    }

    const float pSpec = OpaqueBsdfLobeSelectionProbFromNoV(NoV, f0, albedo, metallic);
    const float3 h = normalize(viewDir + wi);
    const float NoH = saturate(dot(hitNormal, h));

    const float d = GgxD(NoH, alpha);
    const float g1 = SmithG1(NoV, alpha);
    const float pdfSpec = g1 * d / max(4.0 * NoV, 1e-4);
    const float pdfDiff = NoL / kPi;
    return pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;
}

// Computes the same opaque BSDF and mixture pdf as the two public helpers above, but shares only
// per-direction intermediates. This is intentionally local to an environment-DI candidate: no
// surface context survives across the 16-candidate loop and therefore cannot extend raygen live
// state.
void EvaluateOpaqueBsdfAndPdf(
    float3 hitNormal,
    float3 viewDir,
    float3 wi,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    out float3 bsdf,
    out float pdf)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3);
    const float noV = saturate(dot(hitNormal, viewDir));
    const float noL = saturate(dot(hitNormal, wi));
    if (noL <= 0.0)
    {
        bsdf = 0.0.xxx;
        pdf = 0.0;
        return;
    }

    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));
    const float3 fresnelNoV = FresnelSchlick(noV, f0);
    const float pSpec = OpaqueBsdfLobeSelectionProbFromNoV(
        noV, f0, albedo, metallic);

    const float3 h = normalize(viewDir + wi);
    const float noH = saturate(dot(hitNormal, h));
    const float voH = saturate(dot(viewDir, h));
    const float d = GgxD(noH, alpha);
    const float3 fresnel = FresnelSchlick(voH, f0);
    const float3 specCos = d * SmithG2HeightCorrelated(noV, noL, alpha) * fresnel / max(4.0 * noV, 1e-4);
    const float3 diffCos = baseDiffuse * (1.0.xxx - fresnelNoV) * (noL / kPi);
    bsdf = specCos + diffCos;

    const float pdfSpec = SmithG1(noV, alpha) * d / max(4.0 * noV, 1e-4);
    const float pdfDiff = noL / kPi;
    pdf = pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;
}

// Sun NEE — full opaque BSDF toward the sun. Delta light: no MIS partner, weight 1.
// Gate on the SHADING normal (like env/emissive NEE): with normal maps, bump walls stay lit beyond
// the geometric terminator, and a geometric-normal gate chops them with a hard edge exactly at the
// sun-visibility boundary. The traced shadow ray (from the geometric-offset origin) still provides
// real occlusion, so light naturally falls off past the terminator instead of stepping to black.
float3 EvaluateDirectSun(
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 shadingNormal,
    float sunVis)
{
    const float3 sunL = normalize(g_SunDirection);
    if (dot(shadingNormal, sunL) <= 0.0)
    {
        return 0.0.xxx;
    }

    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    const float3 bsdf = EvaluateOpaqueBsdf(shadingNormal, viewDir, sunL, f0, albedo, roughness, metallic);
    return bsdf * sunRadiance * sunVis;
}

// One emissive mesh-light sample per bounce (triangle surface + transmissive shadow ray), full-BSDF,
// MIS-weighted against BSDF sampling with the true mixture pdf (both measured in solid angle).
float3 EvaluateDirectEmissive(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin)
{
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return 0.0.xxx;
    }

    const float4 xiPick = PathRngNext4(rng);
    const float4 xiSurface = PathRngNext4(rng);

    const uint lightIndex = SampleEmissiveAlias(
        g_EmissiveLightAlias, 0u, g_EmissiveLightCount, xiPick.x);

    const EmissiveLightEntry light = g_EmissiveLights[lightIndex];
    if (light.triangleCount == 0u)
    {
        return 0.0.xxx;
    }

    const uint triangleIndex = SampleEmissiveAlias(
        g_EmissiveTriangleAlias, light.triangleOffset, light.triangleCount, xiPick.y);

    const EmissiveTriangleEntry emitterTri = g_EmissiveTriangles[triangleIndex];

    float3 lightPoint;
    float pdfArea;
    SampleUniformPointOnTriangle(
        emitterTri.v0,
        emitterTri.v1,
        emitterTri.v2,
        emitterTri.triangleArea,
        xiSurface.xy,
        lightPoint,
        pdfArea);

    float3 toLight = lightPoint - shadowOrigin;
    const float dist2 = max(dot(toLight, toLight), 1e-8);
    const float dist = sqrt(dist2);
    const float3 wi = toLight / dist;

    const float cosThetaReceiver = saturate(dot(hitNormal, wi));
    if (cosThetaReceiver <= 0.0)
    {
        return 0.0.xxx;
    }

    const float cosThetaEmitter = saturate(dot(emitterTri.faceNormal, -wi));
    if (cosThetaEmitter <= 0.0)
    {
        return 0.0.xxx;
    }

    // Opaque any-hit: glass blocks emissive NEE like a wall. Sun keeps TraceSunNeeVisibility for
    // window→floor lighting; emissive-through-glass via NEE is rarely noticeable and was the
    // glass+emissive perf cliff (a0cc7f8). BSDF paths through glass still find emitters.
    const float visibility = TraceVisibility(shadowOrigin, wi, dist - 0.001);
    if (visibility <= 0.0)
    {
        return 0.0.xxx;
    }

    const float pickPdf = emitterTri.pickWeight / g_EmissiveLightPickWeightSum;
    const float pdfSolidAngle =
        EmissiveNeePdfSolidAngle(pickPdf, emitterTri.triangleArea, dist2, cosThetaEmitter);
    const float pdfBsdf = OpaqueBsdfPdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float misWeight = BalanceHeuristic(pdfSolidAngle, pdfBsdf);

    // EvaluateOpaqueBsdf already returns f * cosThetaReceiver, so the geometry term must carry ONLY
    // the emitter cosine and 1/dist² — multiplying by cosThetaReceiver again dims area lights by an
    // extra NoL and desyncs this NEE estimate from the BSDF-hit side of the MIS pair (C3).
    const float3 bsdf = EvaluateOpaqueBsdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float geometryTerm = cosThetaEmitter / dist2;

    return bsdf * light.emissive * geometryTerm * visibility * misWeight
        / max(pickPdf * pdfArea, 1e-8);
}

// Infinite environment light NEE — luminance-importance sampled HDR equirect, MIS vs BSDF.
float3 EvaluateDirectEnvironment(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin)
{
    if (g_EnvLightImportanceCount == 0u)
    {
        return 0.0.xxx;
    }

    const float4 xi = PathRngNext4(rng);
    float3 wi;
    float pdfEnv;
    if (!SampleEnvLightDirection(xi, wi, pdfEnv))
    {
        return 0.0.xxx;
    }

    if (dot(hitNormal, wi) <= 0.0)
    {
        return 0.0.xxx;
    }

    // Same opaque-fast policy as emissive NEE (see EvaluateDirectEmissive).
    const float visibility = TraceVisibility(shadowOrigin, wi, g_MaxTraceDistance);
    if (visibility <= 0.0)
    {
        return 0.0.xxx;
    }

    const float pdfBsdf = OpaqueBsdfPdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float misWeight = BalanceHeuristic(pdfEnv, pdfBsdf);
    const float3 bsdf = EvaluateOpaqueBsdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float3 radiance = EnvNeeRadiance(wi);
    return bsdf * radiance * visibility * misWeight / max(pdfEnv, 1e-8);
}

// ===== ReSTIR DI initial sampling, M=1 (restir-production-roadmap.md P2) =====
// Emitters and the environment are DISJOINT sources (a direction either hits an emissive triangle or
// escapes to sky), so — exactly like the existing EvaluateDirectEmissive + EvaluateDirectEnvironment
// which are ADDED — they resample as two independent single-proposal RIS reservoirs whose shaded
// results are summed. Each candidate stores its UNSHADOWED contribution f = BSDF·radiance·MIS and its
// proposal pdf (solid angle); one visibility ray validates the reservoir winner. At candidateCount=1
// the RIS UCW is W=1/p, so the shade is BSDF·radiance·MIS·V/p — byte-for-byte the plain NEE above
// (the built-in A/B parity anchor). The resampling math is proven in tests/restir_di_test.cpp.

// Draw one emissive-triangle candidate: fills the UNSHADOWED contribution (BSDF·emissive·MIS), the
// direction/distance for the winner's shadow ray, and the solid-angle proposal pdf. contribution
// stays 0 (a valid zero-target candidate that still counts toward M) on backface/degenerate picks.
void SampleEmissiveDiCandidate(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin,
    out float3 contribution,
    out float3 wi,
    out float shadowDist,
    out float proposalPdf,
    out RestirDiLightSample lightSample)
{
    contribution = 0.0.xxx;
    wi = float3(0.0, 0.0, 1.0);
    shadowDist = 0.0;
    proposalPdf = 0.0;
    lightSample = RestirDiInvalidLightSample();

    const float4 xiPick = PathRngNext4(rng);
    const float4 xiSurface = PathRngNext4(rng);

    const uint lightIndex = SampleEmissiveAlias(
        g_EmissiveLightAlias, 0u, g_EmissiveLightCount, xiPick.x);

    const EmissiveLightEntry light = g_EmissiveLights[lightIndex];
    if (light.triangleCount == 0u)
    {
        return;
    }

    const uint triangleIndex = SampleEmissiveAlias(
        g_EmissiveTriangleAlias, light.triangleOffset, light.triangleCount, xiPick.y);

    const EmissiveTriangleEntry emitterTri = g_EmissiveTriangles[triangleIndex];
    lightSample.sampleType = kRestirDiSampleEmissive;
    lightSample.index0 = lightIndex;
    lightSample.index1 = triangleIndex;
    lightSample.uv = xiSurface.xy;

    float3 lightPoint;
    float pdfArea;
    SampleUniformPointOnTriangle(
        emitterTri.v0, emitterTri.v1, emitterTri.v2, emitterTri.triangleArea,
        xiSurface.xy, lightPoint, pdfArea);

    const float3 toLight = lightPoint - shadowOrigin;
    const float dist2 = max(dot(toLight, toLight), 1e-8);
    const float dist = sqrt(dist2);
    wi = toLight / dist;

    if (saturate(dot(hitNormal, wi)) <= 0.0)
    {
        return;
    }
    const float cosThetaEmitter = saturate(dot(emitterTri.faceNormal, -wi));
    if (cosThetaEmitter <= 0.0)
    {
        return;
    }

    const float pickPdf = emitterTri.pickWeight / g_EmissiveLightPickWeightSum;
    const float pdfSolidAngle =
        EmissiveNeePdfSolidAngle(pickPdf, emitterTri.triangleArea, dist2, cosThetaEmitter);
    if (pdfSolidAngle <= 0.0)
    {
        return;
    }
    const float pdfBsdf = OpaqueBsdfPdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float3 bsdf = EvaluateOpaqueBsdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float misWeight = BalanceHeuristic(pdfSolidAngle, pdfBsdf);

    // f = BSDF·emissive·MIS (the directional integrand × MIS, visibility excluded). With
    // proposalPdf = pdfSolidAngle, M=1 gives f·V/pdfSolidAngle == EvaluateDirectEmissive.
    contribution = bsdf * light.emissive * misWeight;
    shadowDist = dist - 0.001;
    proposalPdf = pdfSolidAngle;
}

// Bounce-0 emissive direct via RIS over `candidateCount` emitter candidates + one shadow ray.
float3 RestirDiEmissiveDirect(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin,
    uint candidateCount,
    out RestirDiTemporalReservoir temporalReservoir)
{
    temporalReservoir = RestirDiTemporalInit();
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0 || candidateCount == 0u)
    {
        return 0.0.xxx;
    }

    RestirDiReservoir res = RestirDiInit();
    [loop]
    for (uint c = 0u; c < candidateCount; ++c)
    {
        float3 contribution;
        float3 wi;
        float shadowDist;
        float proposalPdf;
        RestirDiLightSample lightSample;
        SampleEmissiveDiCandidate(
            rng, viewDir, f0, albedo, roughness, metallic, hitNormal, shadowOrigin,
            contribution, wi, shadowDist, proposalPdf, lightSample);
        // M=1 selection is deterministic. Do not consume an otherwise unused RNG dimension: this
        // keeps DI=1 byte-for-byte aligned with plain one-sample NEE for the environment draw and
        // every later path event (the P2 parity anchor).
        float selectXi = 0.0;
        if (candidateCount > 1u) selectXi = PathRngNext(rng);
        RestirDiUpdate(res, contribution, wi, shadowDist, proposalPdf, selectXi);
        RestirDiTemporalUpdate(
            temporalReservoir, lightSample, RestirDiTargetLuminance(contribution), proposalPdf, selectXi);
    }

    RestirDiFinalize(res);
    RestirDiTemporalFinalize(temporalReservoir);
    if (res.targetPdf <= 0.0)
    {
        return 0.0.xxx;
    }

    // Opaque any-hit visibility (glass blocks emissive NEE), matching EvaluateDirectEmissive.
    const float visibility = TraceVisibility(shadowOrigin, res.direction, res.distance);
    return RestirDiShade(res, visibility);
}

// Bounce-0 environment direct via RIS over `candidateCount` env-direction candidates + one shadow ray.
float3 RestirDiEnvironmentDirect(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin,
    uint candidateCount,
    out RestirDiTemporalReservoir temporalReservoir)
{
    temporalReservoir = RestirDiTemporalInit();
    if (g_EnvLightImportanceCount == 0u || candidateCount == 0u)
    {
        return 0.0.xxx;
    }

    RestirDiReservoir res = RestirDiInit();
#if PT_DIAGNOSTIC_PERMUTATION
    const bool envDiProbeSampling = g_PtDebugIsolateMode == 28u;
    const bool envDiProbeBsdfMis = g_PtDebugIsolateMode == 29u;
    const bool envDiProbeCandidate = g_PtDebugIsolateMode == 30u;
    const bool envDiProbeRadiance = g_PtDebugIsolateMode == 31u;
    const bool envDiProbeMetadata = g_PtDebugIsolateMode == 32u;
    const bool envDiProbeNoReservoir = envDiProbeSampling || envDiProbeBsdfMis || envDiProbeCandidate
        || envDiProbeRadiance || envDiProbeMetadata;
#endif
    [loop]
    for (uint c = 0u; c < candidateCount; ++c)
    {
        float3 contribution = 0.0.xxx;
        float3 wi = float3(0.0, 0.0, 1.0);
        float proposalPdf = 0.0;
        RestirDiLightSample lightSample = RestirDiInvalidLightSample();

        const float4 xi = PathRngNext4(rng);
        float pdfEnv;
        if (SampleEnvLightDirection(xi, wi, pdfEnv) && dot(hitNormal, wi) > 0.0 && pdfEnv > 0.0)
        {
#if PT_DIAGNOSTIC_PERMUTATION
            if (!envDiProbeSampling)
            {
#endif
            float3 bsdf;
            float pdfBsdf;
            EvaluateOpaqueBsdfAndPdf(
                hitNormal, viewDir, wi, f0, albedo, roughness, metallic, bsdf, pdfBsdf);
            const float misWeight = BalanceHeuristic(pdfEnv, pdfBsdf);
#if PT_DIAGNOSTIC_PERMUTATION
            if (!envDiProbeBsdfMis)
            {
#endif
            // f = BSDF·radiance·MIS; with proposalPdf = pdfEnv, M=1 gives EvaluateDirectEnvironment.
#if PT_DIAGNOSTIC_PERMUTATION
            const float3 envRadiance = envDiProbeMetadata ? 0.0.xxx : EnvNeeRadiance(wi);
#else
            const float3 envRadiance = EnvNeeRadiance(wi);
#endif
            contribution = bsdf * envRadiance * misWeight;
            proposalPdf = pdfEnv;
#if PT_DIAGNOSTIC_PERMUTATION
            if (!envDiProbeRadiance)
            {
#endif
            lightSample.sampleType = kRestirDiSampleEnvironment;
            lightSample.uv = DirectionToEquirectUv(wi);
#if PT_DIAGNOSTIC_PERMUTATION
            }
#endif
#if PT_DIAGNOSTIC_PERMUTATION
            }
            }
#endif
        }
        float selectXi = 0.0;
        if (candidateCount > 1u) selectXi = PathRngNext(rng);
#if PT_DIAGNOSTIC_PERMUTATION
        if (!envDiProbeNoReservoir)
        {
#endif
        RestirDiUpdate(res, contribution, wi, g_MaxTraceDistance, proposalPdf, selectXi);
        RestirDiTemporalUpdate(
            temporalReservoir, lightSample, RestirDiTargetLuminance(contribution), proposalPdf, selectXi);
#if PT_DIAGNOSTIC_PERMUTATION
        }
#endif
    }

    RestirDiFinalize(res);
    RestirDiTemporalFinalize(temporalReservoir);
    if (res.targetPdf <= 0.0)
    {
        return 0.0.xxx;
    }

    const float visibility = TraceVisibility(shadowOrigin, res.direction, res.distance);
    return RestirDiShade(res, visibility);
}

// Opaque BRDF bounce: stochastic diffuse/GGX-specular lobe pick, weighted by the FULL BRDF over the
// mixture pdf — the one-sample MIS estimator (Veach 1997 §9.2.4). Specular carries the correct VNDF
// estimator weight F(VoH)*G2/G1 (Heitz 2018), NOT constant f0; diffuse is Fresnel-attenuated by
// (1-F) so diffuse+specular conserve energy (white furnace). scatterPdf returns the mixture
// directional pdf for NEE MIS. No forced-lobe branches — the divisor is always the true selection
// probability, so no lobe is over/under-weighted (B3).
void SampleOpaqueInterface(
    float3 hitNormal,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float2 xi2d,
    float lobeXi,
    out float3 nextDir,
    out bool isSpecular,
    out uint scatterEvent,
    out float scatterPdf,
    inout float3 throughput)
{
    scatterEvent = kPtScatterEventInvalid;
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3); // matches SampleGgxVndfHalfVector
    const float NoV = saturate(dot(hitNormal, viewDir));

    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));

    // View-side Fresnel — reused for the lobe-selection heuristic AND the energy-conserving diffuse
    // weight below. Using F(NoV) (evaluated once) rather than the per-microfacet F(VoH) is what keeps
    // diffuse+specular from gaining energy at grazing angles (verified by the white-furnace test).
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);

    // Lobe selection probability: an importance-sampling heuristic — any value in (0,1) is unbiased
    // (it only affects variance). Balance specular vs diffuse reflectance, bias toward specular for
    // metals. A zero-energy diffuse component is removed from the sampling technique set entirely;
    // otherwise both lobes remain samplable and the mixture pdf stays valid.
    const float pSpec = OpaqueBsdfLobeSelectionProbFromNoV(
        NoV, f0, albedo, metallic);

    const bool sampledSpecular = (lobeXi < pSpec);
    float3 l;
    if (sampledSpecular && roughness <= kPtDeltaSpecularRoughness)
    {
        // Delta mirror / near-mirror: bypass VNDF (its 1e-3 alpha floor reads as frosted).
        l = normalize(reflect(-viewDir, hitNormal));
        isSpecular = true;
        scatterEvent = kPtScatterEventDeltaSpecular;
        nextDir = l;

        const float NoL = dot(hitNormal, l);
        if (NoL <= 0.0)
        {
            scatterEvent = kPtScatterEventInvalid;
            scatterPdf = 1.0;
            throughput = 0.0.xxx;
            return;
        }

        throughput *= fresnelNoV / max(pSpec, 1e-6);
        scatterPdf = kDeltaScatterPdf;
        return;
    }

    if (sampledSpecular)
    {
        const float3 h = SampleGgxVndfHalfVector(hitNormal, viewDir, ggxRoughness, xi2d);
        l = reflect(-viewDir, h);
    }
    else
    {
        l = CosineSampleHemisphere(hitNormal, xi2d);
    }

    isSpecular = sampledSpecular;
    scatterEvent = sampledSpecular
        ? kPtScatterEventGlossySpecular
        : kPtScatterEventDiffuse;
    nextDir = l;

    const float NoL = dot(hitNormal, l);
    if (NoL <= 0.0)
    {
        // Sampled below the horizon: terminate this path sample (unbiased).
        scatterEvent = kPtScatterEventInvalid;
        scatterPdf = 1.0;
        throughput = 0.0.xxx;
        return;
    }

    const float3 h = normalize(viewDir + l);
    const float NoH = saturate(dot(hitNormal, h));
    const float VoH = saturate(dot(viewDir, h));

    const float d = GgxD(NoH, alpha);
    const float g1 = SmithG1(NoV, alpha);
    const float g2 = SmithG2HeightCorrelated(NoV, NoL, alpha);
    const float3 fresnel = FresnelSchlick(VoH, f0);

    // BRDF * cos(theta_l). Specular uses per-microfacet F(VoH); diffuse uses view-side F(NoV) so the
    // two lobes conserve energy (a per-microfacet (1-F(VoH)) diffuse gains ~9% at grazing).
    const float3 specCos = d * g2 * fresnel / max(4.0 * NoV, 1e-4);              // f_spec * NoL
    const float3 diffCos = baseDiffuse * (1.0.xxx - fresnelNoV) * (NoL / kPi);   // f_diff * NoL, (1-F(NoV)) split

    // Mixture directional pdf: VNDF for specular (= G1*D/(4*NoV)), cosine for diffuse.
    const float pdfSpec = g1 * d / max(4.0 * NoV, 1e-4);
    const float pdfDiff = NoL / kPi;
    const float pdfMix = pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;

    throughput *= (specCos + diffCos) / max(pdfMix, 1e-9);
    scatterPdf = pdfMix;
}

// Unified material bounce: dielectric Fresnel/Snell or opaque GGX.
//
// G2: Fresnel at a dielectric interface is always stochastic (reflect XOR refract) — never the old
// real-time dual inject. That is what ReSTIR needs for sample~pdf on glass.
//
// Glass vs opaque mixture (dielectricWeight = transmission·(1−metallic)):
//   Reference — Bernoulli pick with 1/p (unbiased; fireflies average out under accumulation).
//   Real-time — always follow the glass lobe when dw > 0 and scale throughput by dw (p=1 for that
//   choice). Opaque residual stays on NEE/ambient via opaqueWeight. This avoids the 1/dw mid-t
//   blowout at 1 spp; the path is still a single dielectric sample with a well-defined pdf.
bool SampleMaterialBounce(
    inout PathRng rng,
    float3 hitNormal,
    float3 rayDir,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float transmission,
    float ior,
    bool thinWalled,
    bool pathInMedium,
    bool useFirstOpticalInterface,
    FirstOpticalInterface firstOpticalInterface,
    out float3 nextDir,
    out bool isSpecular,
    out bool outPathInMedium,
    out float scatterPdf,
    out uint scatterEvent,
    out uint actualOpticalEvent,
    inout float3 throughput)
{
    const float dielectricWeight = DielectricWeight(transmission, metallic);
    const float4 xi = PathRngNext4(rng);

    outPathInMedium = pathInMedium;
    isSpecular = false;
    scatterPdf = 1.0;
    scatterEvent = kPtScatterEventInvalid;
    actualOpticalEvent = kFirstOpticalEventNone;

    const bool useGlassPath = dielectricWeight > 0.0
        && (kPtCenterPrimaryRays || xi.w < dielectricWeight);

    if (useGlassPath)
    {
        if (kPtCenterPrimaryRays)
        {
            throughput *= dielectricWeight;
        }
        else
        {
            throughput /= max(dielectricWeight, 1e-6);
        }
        if (useFirstOpticalInterface)
        {
            SampleFirstOpticalInterface(
                firstOpticalInterface,
                xi.z,
                nextDir,
                outPathInMedium,
                scatterPdf,
                actualOpticalEvent);
        }
        else
        {
            SampleDielectricInterface(
                hitNormal,
                rayDir,
                roughness,
                ior,
                thinWalled,
                pathInMedium,
                xi.z,
                xi.xy,
                nextDir,
                outPathInMedium,
                scatterPdf);
        }
        isSpecular = true;
        scatterEvent = kPtScatterEventOptical;
        return true;
    }

    if (dielectricWeight > 0.0)
    {
        throughput /= max(1.0 - dielectricWeight, 1e-6);
    }

    SampleOpaqueInterface(
        hitNormal,
        viewDir,
        f0,
        albedo,
        roughness,
        metallic,
        xi.xy,
        xi.z,
        nextDir,
        isSpecular,
        scatterEvent,
        scatterPdf,
        throughput);
    return isSpecular;
}

#if PT_DIAGNOSTIC_PERMUTATION
float3 SelectPtDebugRadiance(
    uint isolateMode,
    bool primaryHit,
    float3 radiance,
    float3 radiancePreClamp,
    float3 termDirectSun,
    float3 termDirectEmissive,
    float3 termSurfaceEmissive,
    float3 termAmbient,
    float3 termIndirect,
    float primaryAoVis,
    float primarySunVis,
    float specHitDistGuide)
{
    if (isolateMode == 0u)
    {
        return radiance;
    }

    // Sky pixels: only show the environment for views that include miss/sky transport.
    // Surface-only terms use black sky so they do not look like the full composite.
    if (!primaryHit)
    {
        if (isolateMode == 7u || isolateMode == 8u)
        {
            return radiancePreClamp;
        }
        return 0.0.xxx;
    }

    if (isolateMode == 1u)
    {
        return termDirectSun;
    }
    if (isolateMode == 2u)
    {
        return termDirectEmissive * float3(1.0, 0.65, 1.0);
    }
    if (isolateMode == 3u)
    {
        return termSurfaceEmissive;
    }
    if (isolateMode == 4u)
    {
        return termAmbient * float3(0.65, 0.8, 1.0);
    }
    if (isolateMode == 5u)
    {
        const float v = saturate(primaryAoVis * 4.0);
        return float3(v * 0.2, v, v * 0.25);
    }
    if (isolateMode == 6u)
    {
        const float v = saturate(primarySunVis * 4.0);
        return float3(v, v * 0.85, v * 0.15);
    }
    if (isolateMode == 7u)
    {
        // G5: structural indirect = throughputAfterFirstScatter * Lo_tail (not residual subtract).
        return termIndirect;
    }
    if (isolateMode == 8u)
    {
        return radiancePreClamp;
    }
    if (isolateMode == 9u)
    {
        const float normalizedDist = saturate(specHitDistGuide / max(g_MaxTraceDistance, 1e-4));
        const float v = 1.0 - normalizedDist;
        return float3(v, v * 0.35, 0.0);
    }

    return radiancePreClamp;
}
#endif

[shader("raygeneration")]
void PathTracerRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const uint pixelIndex = pixel.y * g_OutputSize.x + pixel.x;

    // Dense neutral defaults make feature-off and non-PSR pixels explicit. PSR ownership is
    // published only after at least one exact link is accepted.
    g_PsrThroughput[pixel] = float4(1.0, 1.0, 1.0, 0.0);
    g_PsrMetadata[pixel] = PackPtPsrMetadata(
        0u, kPtPsrTerminalPrimaryReceiver, false, false);
    g_SpecularMotion[pixel] = 0.0.xx;

    // Layer 1 is intentionally empty for opaque, rough, unsupported, mirror-only, and sky pixels.
    // Neutral guides make those black samples a coherent far-field domain for the independent RR
    // history without changing the established single-layer path.
    if (!kPtLegacyOpticalRouting)
    {
        g_OpticalTransmissionOutput[pixel] = 0.0.xxxx;
        g_OpticalTransmissionDepth[pixel] = 1.0;
        g_OpticalTransmissionMotion[pixel] = 0.0.xxxx;
        g_OpticalTransmissionDiffuseAlbedo[pixel] = float4(0.5, 0.5, 0.5, 1.0);
        g_OpticalTransmissionSpecularAlbedo[pixel] = float4(0.5, 0.5, 0.5, 1.0);
        g_OpticalTransmissionNormalRoughness[pixel] = float4(0.0, 0.0, 1.0, 1.0);
    }

    PathRng rng = InitPathRng(pixel, g_FrameIndex);
    const uint pathSeed = rng.seed;
    const float4 primaryXi = PathRngNext4(rng);
    const float2 primaryOffset = kPtCenterPrimaryRays ? float2(0.5, 0.5) : primaryXi.zw;
    const float2 texCoord = (float2(pixel) + primaryOffset) / float2(g_OutputSize);
    const float2 clipXY = PixelToClipXY(texCoord);

    const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 farWorld = farH.xyz / farH.w;
    const float3 cameraRayDir = normalize(farWorld - g_CameraPos);

    // Match the host clamp (DxrSettings 1..16) and the reflection/GI passes; previously capped at 8,
    // so slider values 9..16 silently did nothing.
    const uint maxBounces = clamp(g_MaxBounces, 1u, 16u);

    // G5: bounce-0 direct vs Lo_tail (bounce≥1) with local throughput reset after first scatter.
    float3 directRadiance = 0.0.xxx;
    float3 loTail = 0.0.xxx;
    float3 transmissionLoTail = 0.0.xxx;
    float3 throughput = 1.0.xxx;
    float3 throughputAfterFirstScatter = 1.0.xxx;
    bool inTail = false;
    bool haveInitialSample = false;
    float3 sampleXs = 0.0.xxx;
    float3 sampleNs = float3(0.0, 1.0, 0.0);
    float samplePdf = 1.0;
    uint sampleFlags = 0u;
    uint sampleInstanceId = 0u;
    uint samplePrimitiveIndex = 0u;
    PtPrimaryReconnectState primary;
    primary.hit = false;
    primary.roughness = 1.0;
    primary.dielectricWeight = 0.0;
    primary.worldPos = 0.0.xxx;
    primary.geomNormal = float3(0.0, 1.0, 0.0);
    primary.shadingNormal = float3(0.0, 1.0, 0.0);
    primary.albedo = 0.0.xxx;
    primary.metallic = 0.0;
    RestirDiReservoirSet freshDiReservoirs;
    freshDiReservoirs.emissive = RestirDiTemporalInit();
    freshDiReservoirs.environment = RestirDiTemporalInit();
    float3 freshDiRadiance = 0.0.xxx;

    RayDesc ray;
    ray.Origin = g_CameraPos;
    ray.Direction = cameraRayDir;
    ray.TMin = 0.001;
    ray.TMax = g_MaxTraceDistance;

    float specHitDistGuide = g_MaxTraceDistance;
    bool mirrorChainActive = false;
    uint mirrorChainLength = 0u;
    float mirrorChainDistance = 0.0;
    float3 mirrorChainThroughput = 1.0.xxx;
    float mirrorProjectedSpanPx = 0.0;
    bool mirrorProjectionValid = false;
    bool psrOwned = false;
    bool psrGlassReceiver = false;
    uint psrTerminalReason = kPtPsrTerminalPrimaryReceiver;
    PtMirrorVirtualTransform mirrorVirtualTransform = InitPtMirrorVirtualTransform();
#if PT_DIAGNOSTIC_PERMUTATION
    bool mirrorOwnerValidForDebug = false;
    bool mirrorOwnerSkyForDebug = false;
    uint mirrorOwnerChainLengthForDebug = 0u;
    float mirrorOwnerConfidenceForDebug = 0.0;
    uint mirrorOwnerInstanceIdForDebug = 0u;
    float mirrorOwnerLinearDepthForDebug = 0.0;
    float2 mirrorOwnerMotionForDebug = 0.0.xx;
    bool mirrorBounceCapFallbackForDebug = false;
    bool mirrorNonDeltaFallbackForDebug = false;
    float mirrorGlossyConfidenceForDebug = 0.0;
    bool mirrorGlossyPendingForDebug = false;
    float mirrorGlossyRoughnessForDebug = 1.0;
    float mirrorGlossyPdfForDebug = 0.0;
    uint primaryMaterialIdForDebug = 0u;
    float2 opaquePrimaryMotionForDebug = 0.0.xx;
    float2 transmissionVirtualMotionForDebug = 0.0.xx;
    bool hasTransmissionVirtualMotionForDebug = false;
    bool hasFirstOpticalInterfaceForDebug = false;
    FirstOpticalInterface firstOpticalInterfaceForDebug;
    uint actualOpticalEventForDebug = kFirstOpticalEventNone;
    uint guideReceiverIdForDebug = 0u;
    uint opticalFallbackFlagsForDebug = 0u;
    float3 opticalReceiverReprojectionForDebug = 0.0.xxx;
    float3 reflectionReceiverReprojectionForDebug = 0.0.xxx;
    float3 transmissionReceiverReprojectionForDebug = 0.0.xxx;
    float3 reflectionReplayStatusForDebug = 0.0.xxx;
    float3 transmissionReplayStatusForDebug = 0.0.xxx;
    bool deterministicSplitAcceptedForDebug = false;
    bool transmissionContinuationResumedForDebug = false;
    bool transmissionReceiverShadedForDebug = false;
    float3 transmissionEnvironmentForDebug = 0.0.xxx;
    float3 transmissionReceiverForDebug = 0.0.xxx;
    float3 transmissionDeepBounceForDebug = 0.0.xxx;
#endif
    // Ray-cone width for albedo texture LOD, grown by pixel spread × distance along the path.
    float pathConeWidth = 0.0;
    // Roughness of the surface that launched the current ray — drives env mip on miss, matching
    // reflections.hlsl (payload.surfaceRoughness). Previously bounce>=1 always used 1.0, which
    // made mirror sky reflections black while primary camera misses (roughness 0) still worked.
    float missEnvRoughness = 0.0;
    // Real-time only: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md v2) covers
    // the diffuse sky floor, so a DIFFUSE bounce that escapes must NOT also add the environment.
    // SPECULAR bounces still add the env on miss (background + reflections). Reference traces the sky
    // purely and always adds it on miss.
    bool addEnvOnMiss = true;
    float lastScatterPdf = kDeltaScatterPdf;
    bool pathInMedium = false;
    float3 mediumTint = 1.0.xxx;
    // A deterministic smooth-glass primary launches reflection first, then resumes this stored
    // transmission tail whenever the reflection path terminates.  Both tails accumulate with their
    // physical Fresnel weights, so no per-frame branch choice reaches DLSS-RR.
    bool pendingPrimaryTransmissionTail = false;
    bool tracingPrimaryTransmissionLayer = false;
    RayDesc pendingTransmissionRay;
    float3 pendingTransmissionThroughput = 0.0.xxx;
    bool pendingTransmissionPathInMedium = false;
    float3 pendingTransmissionMediumTint = 1.0.xxx;
    float pendingTransmissionConeWidth = 0.0;
    float pendingTransmissionMissRoughness = 0.0;
    bool pendingTransmissionAddEnvOnMiss = true;

#if PT_DIAGNOSTIC_PERMUTATION
    float3 termDirectSun = 0.0.xxx;
    float3 termDirectEmissive = 0.0.xxx;
    float3 termSurfaceEmissive = 0.0.xxx;
    float3 termAmbient = 0.0.xxx;
    float primaryAoVis = 1.0;
    float primarySunVis = 0.0;
#endif

    uint bounce = 0u;
    const uint maxTraceSteps = maxBounces + (kPtMirrorChainPsrEnabled ? g_PtPsrMaxBounces : 0u);
    [loop]
    for (uint traceStep = 0u; traceStep <= maxTraceSteps; ++traceStep)
    {
        Payload payload;
        ResetPayload(payload);
        // G7/P2: primary needs motion/depth; every shading bounce needs LOD/bary/normal-map.
        payload.hit = kPayloadReqShadingData;
        bool requestMirrorReceiverData = mirrorChainActive;
#if PT_DIAGNOSTIC_PERMUTATION
        requestMirrorReceiverData = requestMirrorReceiverData || mirrorGlossyPendingForDebug;
#endif
        if (bounce == 0u || (kPtMirrorChainPsrEnabled && requestMirrorReceiverData))
        {
            payload.hit |= kPayloadReqPrimarySurface;
        }
        TracePathRay(ray, payload);

        if (payload.hit == 0)
        {
            if (kPtMirrorChainPsrEnabled && mirrorChainActive)
            {
                const PtRrGuideOwner skyOwner = BuildPtRrSkyGuideOwner(
                    ray.Direction,
                    mirrorVirtualTransform,
                    throughputAfterFirstScatter * throughput,
                    mirrorChainLength,
                    g_MaxTraceDistance);
                CommitPtRrGuideOwner(pixel, skyOwner, specHitDistGuide);
                psrOwned = skyOwner.valid;
                psrTerminalReason = skyOwner.valid
                    ? kPtPsrTerminalEnvironmentEscape
                    : kPtPsrTerminalInvalidProjectionFallback;
                if (psrOwned)
                {
                    g_PsrThroughput[pixel] = float4(mirrorChainThroughput, 1.0);
                }
                g_PsrMetadata[pixel] = PackPtPsrMetadata(
                    mirrorChainLength,
                    psrTerminalReason,
                    skyOwner.valid,
                    mirrorProjectionValid);
#if PT_DIAGNOSTIC_PERMUTATION
                mirrorOwnerValidForDebug = skyOwner.valid;
                mirrorOwnerSkyForDebug = skyOwner.valid;
                mirrorOwnerChainLengthForDebug = skyOwner.chainLength;
                mirrorOwnerConfidenceForDebug = skyOwner.confidence;
                mirrorOwnerLinearDepthForDebug = skyOwner.linearDepth;
                mirrorOwnerMotionForDebug = skyOwner.motionNdc;
                mirrorNonDeltaFallbackForDebug = mirrorNonDeltaFallbackForDebug || !skyOwner.valid;
#endif
                mirrorChainActive = false;
            }
#if PT_DIAGNOSTIC_PERMUTATION
            if (kPtMirrorChainPsrEnabled && mirrorGlossyPendingForDebug)
            {
                const float skyDistance = g_MaxTraceDistance * 0.5;
                mirrorGlossyConfidenceForDebug = ComputePtGlossyGuideConfidence(
                    mirrorGlossyRoughnessForDebug,
                    mirrorGlossyPdfForDebug,
                    pathConeWidth + g_PtPixelSpreadAngle * skyDistance,
                    skyDistance,
                    skyDistance);
                mirrorGlossyPendingForDebug = false;
                mirrorNonDeltaFallbackForDebug = true;
            }
#endif
            if (bounce == 0u && !psrOwned)
            {
                // Sky pixel: camera-only reprojection (raster sky keeps MV=0; PT supplies finite anchor).
                const float3 skyAnchor = g_CameraPos + ray.Direction * (g_MaxTraceDistance * 0.5);
                float4 currClipUnj = mul(g_UnjitteredViewProj, float4(skyAnchor, 1.0));
                float4 prevClipUnj = mul(g_PrevViewProj, float4(skyAnchor, 1.0));
                if (!kPtMotionHistoryValid)
                {
                    prevClipUnj = currClipUnj;
                }
                const float2 skyMotion = ComputeMotionNdc(currClipUnj, prevClipUnj);

                // Sky RR guides: (0.5, 0.5, 0.5) albedo per the DLSS-RR Integration Guide §3.4.2
                // (white/black diffuse and zero specular are known-bad — devdoc/dxr/pt/sky-motion.md).
                g_DiffuseAlbedoGuide[pixel] = float4(0.5, 0.5, 0.5, 1.0);
                g_SpecularAlbedoGuide[pixel] = float4(0.5, 0.5, 0.5, 1.0);
                g_NormalRoughnessGuide[pixel] = float4(0.0, 0.0, 1.0, 1.0);
                g_RestirSurfacePositionDepth[pixel] = 0.0.xxxx;
                g_RestirSurfaceMaterial[pixel] = 0u.xxxx;
                g_RestirSurfaceAlbedoMetallic[pixel] = 0.0.xxxx;
                g_DepthOutput[pixel] = 1.0;
                g_MotionOutput[pixel] = float4(skyMotion, 0.0, 1.0);
                g_SpecularMotion[pixel] = skyMotion;
                g_Metadata[pixel] = uint2(0, 0);
            }

            float3 missRadiance = 0.0.xxx;
            if (g_EnvLightImportanceCount > 0u)
            {
                // Env NEE active: the miss-side env is the BSDF-sampling half of the env-lighting MIS
                // pair (or the direct sky for a camera/delta ray, where lastScatterPdf =
                // kDeltaScatterPdf → weight ≈ 1). Sample the SHARP equirect — the same radiance the
                // NEE side sees — so both strategies estimate one integrand (also removes the R1
                // prefilter double-blur), and MIS-weight against the env NEE density. Previously the
                // miss added the FULL env: specular double-counted in both modes, diffuse
                // double-counted in reference and under-counted in real-time (C2).
                const float envMisPdf = EnvNeePdfForDirection(ray.Direction);
                const float envMisWeight =
                    envMisPdf > 0.0 ? BalanceHeuristic(lastScatterPdf, envMisPdf) : 1.0;
                // Camera rays retain the authored HDR background. Scattered transport removes an
                // embedded HDR sun when the analytic sun is active, preventing two directional
                // lights and two shadows while keeping the importance PDF conservative.
                const float3 envRadiance = bounce == 0u
                    ? SampleEnvEquirectRadiance(ray.Direction)
                    : ClampEmbeddedSunForTransport(SampleEnvEquirectRadiance(ray.Direction));
                missRadiance = envRadiance * envMisWeight;
            }
            else if (addEnvOnMiss)
            {
                missRadiance = SampleEnvironment(ray.Direction, missEnvRoughness);
            }

            const float3 missContrib = throughput * missRadiance;
            if (inTail)
            {
                loTail += missContrib;
                if (tracingPrimaryTransmissionLayer)
                {
                    transmissionLoTail += missContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                    if (transmissionReceiverShadedForDebug)
                        transmissionDeepBounceForDebug += missContrib;
                    else
                        transmissionEnvironmentForDebug += missContrib;
#endif
                }
                if (!haveInitialSample)
                {
                    sampleFlags |= kRestirSampleNoReuse;
                }
            }
            else
            {
                directRadiance += missContrib;
            }
            if (pendingPrimaryTransmissionTail)
            {
                ray = pendingTransmissionRay;
                throughput = pendingTransmissionThroughput;
                pathInMedium = pendingTransmissionPathInMedium;
                mediumTint = pendingTransmissionMediumTint;
                pathConeWidth = pendingTransmissionConeWidth;
                missEnvRoughness = pendingTransmissionMissRoughness;
                addEnvOnMiss = pendingTransmissionAddEnvOnMiss;
                lastScatterPdf = kDeltaScatterPdf;
                pendingPrimaryTransmissionTail = false;
                tracingPrimaryTransmissionLayer = true;
#if PT_DIAGNOSTIC_PERMUTATION
                transmissionContinuationResumedForDebug = true;
                transmissionReceiverShadedForDebug = false;
#endif
                // The loop increment resumes the stored secondary path at bounce one without
                // re-shading the primary surface or duplicating its direct contribution.
                bounce = 1u;
                continue;
            }
            break;
        }

        pathConeWidth += g_PtPixelSpreadAngle * payload.hitDistance;

        const MaterialEntry material = LoadMaterialForInstance(payload.instanceId);
        const float3 hitNormalGeom = PayloadGeomNormal(payload);
        const float3 hitNormal = PayloadShadingNormal(payload);
        const float albedoLod = ComputeAlbedoLod(payload, pathConeWidth, ray.Direction);
        float3 albedo;
        float surfaceRoughness;
        float surfaceMetallic;
        float3 surfaceEmissiveColor;
        ResolveSurfaceMaterialScalars(
            payload.instanceId,
            payload.primitiveIndex,
            PayloadBarycentrics(payload),
            albedoLod,
            albedo,
            surfaceRoughness,
            surfaceMetallic,
            surfaceEmissiveColor);
        const float3 viewDir = -ray.Direction;
        const float3 hitPos = ray.Origin + ray.Direction * payload.hitDistance;
        const float3 shadowOrigin = hitPos + hitNormalGeom * max(payload.hitDistance * 0.001, 0.002);

        if (kPtMirrorChainPsrEnabled && mirrorChainActive)
        {
            mirrorChainDistance += payload.hitDistance;
        }

        // Deterministic PSR prefix. This classification happens before any NEE, material-scatter
        // RNG, or roulette. Accepted mirror links therefore consume neither the ordinary lighting
        // budget nor the path RNG sequence. The receiver below is shaded as lighting bounce zero.
        const float psrDielectricWeight = DielectricWeight(material.transmission, surfaceMetallic);
        const float3 psrF0 = lerp(0.04.xxx, albedo, surfaceMetallic);
        const bool exactPsrMirror = kPtMirrorChainPsrEnabled
            && IsPtExactPlanarDeltaMirror(
                payload, albedo, surfaceRoughness, surfaceMetallic, psrDielectricWeight);
        if (kPtMirrorChainPsrEnabled && (mirrorChainActive || bounce == 0u) && exactPsrMirror)
        {
            if (!mirrorChainActive)
            {
                CommitPtPsrPrimaryFallback(
                    pixel, payload, hitPos, hitNormalGeom, hitNormal, albedo, surfaceRoughness,
                    surfaceMetallic, material.transmission, material.indexOfRefraction, viewDir);
            }

            float projectedSpan = 0.0;
            const bool projectionValid = g_PtPsrSubpixelThreshold > 0.0
                && ProjectPtPsrMirrorBounds(
                    payload.instanceId, mirrorVirtualTransform, projectedSpan);
            mirrorProjectionValid = mirrorProjectionValid || projectionValid;
            mirrorProjectedSpanPx = projectionValid ? projectedSpan : mirrorProjectedSpanPx;
            const float nDotV = saturate(dot(hitNormal, viewDir));
            const float3 linkThroughput = max(FresnelSchlick(nDotV, psrF0), 0.0.xxx);

            const bool subpixelTerminal = projectionValid
                && g_PtPsrSubpixelThreshold > 0.0
                && projectedSpan <= g_PtPsrSubpixelThreshold;
            const bool hardCapTerminal = mirrorChainLength >= g_PtPsrMaxBounces;
            if (subpixelTerminal || hardCapTerminal)
            {
                const float3 tailDirection = normalize(reflect(ray.Direction, hitNormal));
                const float3 tailThroughput = mirrorChainThroughput * linkThroughput;
                const float3 filteredTail = SampleEnvironment(
                    tailDirection, max(surfaceRoughness, 0.5));
                directRadiance += tailThroughput * filteredTail;
                psrTerminalReason = subpixelTerminal
                    ? kPtPsrTerminalSubpixelTail
                    : kPtPsrTerminalHardCapSignificant;
                g_PsrMetadata[pixel] = PackPtPsrMetadata(
                    mirrorChainLength + 1u, psrTerminalReason, true, projectionValid);
#if PT_DIAGNOSTIC_PERMUTATION
                mirrorBounceCapFallbackForDebug = hardCapTerminal;
#endif
                // A filtered terminal has no finite receiver domain. Preserve the primary fallback
                // guides and keep its already-attenuated radiance out of PSR demodulation.
                psrOwned = false;
                mirrorChainActive = false;
                break;
            }

            if (!AppendPtMirrorPlane(mirrorVirtualTransform, hitPos, hitNormal))
            {
                const float3 tailDirection = normalize(reflect(ray.Direction, hitNormal));
                directRadiance += mirrorChainThroughput * linkThroughput
                    * SampleEnvironment(tailDirection, max(surfaceRoughness, 0.5));
                psrOwned = false;
                psrTerminalReason = kPtPsrTerminalInvalidProjectionFallback;
                g_PsrMetadata[pixel] = PackPtPsrMetadata(
                    mirrorChainLength, psrTerminalReason, false, projectionValid);
                mirrorChainActive = false;
                break;
            }

            mirrorChainActive = true;
            psrOwned = true;
            mirrorChainLength += 1u;
            mirrorChainThroughput *= linkThroughput;
            psrTerminalReason = kPtPsrTerminalReceiver;
            const float originBias = max(payload.hitDistance * 0.0015, 0.01);
            ray.Direction = normalize(reflect(ray.Direction, hitNormal));
            ray.Origin = hitPos + ray.Direction * originBias;
            ray.TMin = 0.001;
            ray.TMax = g_MaxTraceDistance;
            missEnvRoughness = 0.0;
            lastScatterPdf = kDeltaScatterPdf;
            continue;
        }

        if (kPtMirrorChainPsrEnabled && mirrorChainActive)
        {
            // The first non-mirror hit is the PSR receiver. Static glass is a supported receiver:
            // its reflection and transmission lobes are resolved below from this virtual primary
            // interface. Moving receivers still lack previous virtual geometry in this version.
            if (PayloadInstanceMoved(payload))
            {
                directRadiance += mirrorChainThroughput
                    * SampleEnvironment(ray.Direction, max(surfaceRoughness, 0.5));
                psrOwned = false;
                psrTerminalReason = kPtPsrTerminalIneligibleLinkFallback;
                g_PsrMetadata[pixel] = PackPtPsrMetadata(
                    mirrorChainLength, psrTerminalReason, false, mirrorProjectionValid);
                mirrorChainActive = false;
                break;
            }

            const PtRrGuideOwner receiverOwner = BuildPtRrSurfaceGuideOwner(
                payload, hitPos, hitNormal, albedo, surfaceRoughness, surfaceMetallic,
                material.transmission, material.indexOfRefraction, mirrorVirtualTransform,
                mirrorChainThroughput, mirrorChainLength, mirrorChainDistance);
            CommitPtRrGuideOwner(pixel, receiverOwner, specHitDistGuide);
            if (!receiverOwner.valid)
            {
                directRadiance += mirrorChainThroughput
                    * SampleEnvironment(ray.Direction, max(surfaceRoughness, 0.5));
                psrOwned = false;
                psrTerminalReason = kPtPsrTerminalInvalidProjectionFallback;
                g_PsrMetadata[pixel] = PackPtPsrMetadata(
                    mirrorChainLength, psrTerminalReason, false, mirrorProjectionValid);
                mirrorChainActive = false;
                break;
            }
            psrOwned = true;
            psrGlassReceiver = psrDielectricWeight > 0.0;
            psrTerminalReason = kPtPsrTerminalReceiver;
            g_PsrThroughput[pixel] = float4(mirrorChainThroughput, 1.0);
            g_PsrMetadata[pixel] = PackPtPsrMetadata(
                mirrorChainLength, psrTerminalReason, true, mirrorProjectionValid);
            mirrorChainActive = false;
#if PT_DIAGNOSTIC_PERMUTATION
            mirrorOwnerValidForDebug = true;
            mirrorOwnerChainLengthForDebug = mirrorChainLength;
            mirrorOwnerConfidenceForDebug = 1.0;
            mirrorOwnerInstanceIdForDebug = receiverOwner.instanceId;
            mirrorOwnerLinearDepthForDebug = receiverOwner.linearDepth;
            mirrorOwnerMotionForDebug = receiverOwner.motionNdc;
#endif
        }
#if PT_DIAGNOSTIC_PERMUTATION
        if (kPtMirrorChainPsrEnabled && mirrorGlossyPendingForDebug)
        {
            const float receiverLinearDepth = abs(mul(g_WorldToView, float4(hitPos, 1.0)).z);
            mirrorGlossyConfidenceForDebug = ComputePtGlossyGuideConfidence(
                mirrorGlossyRoughnessForDebug,
                mirrorGlossyPdfForDebug,
                pathConeWidth,
                payload.hitDistance,
                receiverLinearDepth);
            mirrorGlossyPendingForDebug = false;
            mirrorNonDeltaFallbackForDebug = true;
        }
#endif

        if (pathInMedium && bounce > 0u)
        {
            throughput *= BeerLambertMediumAttenuation(mediumTint, payload.hitDistance);
        }

        // First indirect vertex (xs): record once when the post-primary scatter lands.
        const bool firstIndirectVertex = inTail && !haveInitialSample;
        if (firstIndirectVertex)
        {
            sampleXs = hitPos;
            sampleNs = hitNormalGeom;
            samplePdf = lastScatterPdf;
            sampleInstanceId = payload.instanceId;
            samplePrimitiveIndex = payload.primitiveIndex;
            haveInitialSample = true;
        }

        const float3 f0 = lerp(0.04.xxx, albedo, surfaceMetallic);
        const float dielectricWeight =
            DielectricWeight(material.transmission, surfaceMetallic);
        if (firstIndirectVertex
            && (dielectricWeight > 0.01 || surfaceRoughness <= kPtDeltaSpecularRoughness))
        {
            // RTXDI treats delta secondary shading as a separate baseline output. Its directional
            // tail is not a reconnectable diffuse/rough GI sample.
            sampleFlags |= kRestirSampleNoReuse;
        }
        const float opaqueWeight = 1.0 - dielectricWeight;
        const float3 specularEnergy =
            FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(surfaceRoughness, 0.55));
        const float3 diffuseAlbedo =
            albedo * (1.0.xxx - specularEnergy) * (1.0 - surfaceMetallic) * (1.0 - dielectricWeight);

        const float emissiveLuminance =
            max(surfaceEmissiveColor.r, max(surfaceEmissiveColor.g, surfaceEmissiveColor.b));
        if (firstIndirectVertex && emissiveLuminance > 1e-4)
        {
            // A BSDF-hit emitter carries primary-domain MIS against bounce-0 NEE. Do not expose
            // that source-PDF-dependent radiance to P6 as a reusable secondary-lighting sample.
            sampleFlags |= kRestirSampleNoReuse;
        }
        // Real-time: treat emitters as path terminals. Staring at a bright cube was paying full
        // soft-sun + emissive + env NEE from the light surface and continuing high-throughput
        // bounces — view-dependent ~2× PT cost with no glass. Reference keeps full paths.
        const bool terminalEmissiveHit = kPtCenterPrimaryRays && emissiveLuminance > 1e-4;
        const bool realTimePrimaryEmitterDisplay = terminalEmissiveHit && bounce == 0u;
        if (emissiveLuminance > 1e-4 && !realTimePrimaryEmitterDisplay)
        {
            float emitterPickPdf;
            float emitterArea;
            EmissiveLightLookup(payload.instanceId, emitterPickPdf, emitterArea);
            // Triangle NEE only samples the emitter's winding front face, so a BSDF ray landing on the
            // BACK face has no NEE partner and must take full weight — otherwise emissive panel back
            // faces render MIS-dimmed (C6). Emission itself stays two-sided.
            const float cosEmitter = saturate(dot(hitNormal, viewDir));
            const float pdfNee = (!PayloadHitBackFace(payload))
                ? EmissiveNeePdfSolidAngle(
                    emitterPickPdf, emitterArea, payload.hitDistance * payload.hitDistance, cosEmitter)
                : 0.0;
            const float misHit = pdfNee > 0.0 ? BalanceHeuristic(lastScatterPdf, pdfNee) : 1.0;
            const float3 surfaceEmissive = surfaceEmissiveColor * misHit;
            const float3 emissiveContrib = throughput * surfaceEmissive;
            if (inTail)
            {
                loTail += emissiveContrib;
                if (tracingPrimaryTransmissionLayer)
                {
                    transmissionLoTail += emissiveContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                    if (transmissionReceiverShadedForDebug)
                        transmissionDeepBounceForDebug += emissiveContrib;
                    else
                        transmissionReceiverForDebug += emissiveContrib;
#endif
                }
            }
            else
            {
                directRadiance += emissiveContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                termSurfaceEmissive += emissiveContrib;
#endif
            }
        }

        // G7/P5: full soft-sun cone at the primary; 1 sample on the Lo_tail (and deeper).
        // Skip NEE on pure dielectrics. Real-time emissive terminals still get sun shading on the
        // emitter surface (faces read as lit), but skip self-emissive/env NEE and further bounces.
        float3 sunPathContrib = 0.0.xxx;
        float3 emissiveNeeContrib = 0.0.xxx;
        float3 envNeeContrib = 0.0.xxx;
        float sunVis = 0.0;
        if (opaqueWeight > 0.0)
        {
            if (!terminalEmissiveHit || bounce == 0u)
            {
                // Emitter terminals: 1 hard/soft sample is enough for face shading; full cone is for
                // ordinary surfaces. Avoids the stare-at-cube soft-sun tax.
                const uint softSunSamples = terminalEmissiveHit
                    ? 1u
                    : ((bounce == 0u) ? kPtSoftSunSampleCount : kPtSoftSunSampleCountDeep);
                sunVis = TracePrimarySunVisibility(rng, hitNormal, shadowOrigin, softSunSamples);
                const float3 sunContrib = opaqueWeight
                    * EvaluateDirectSun(
                        viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                        hitNormal, sunVis);
                sunPathContrib = throughput * sunContrib;
            }
            else
            {
                // A reflected/refracted real-time ray that reaches an emitter must still see the
                // emitter's base material under the scene lighting. Returning emission alone turns
                // a white, red-emissive cube into a flat red card through mirrors and glass. Keep
                // the terminal cost bounded, but spend a visually stable four-sample sun estimate.
                sunVis = TracePrimarySunVisibility(
                    rng, hitNormal, shadowOrigin, kPtSoftSunSampleCount);
                const float3 sunContrib = opaqueWeight
                    * EvaluateDirectSun(
                        viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                        hitNormal, sunVis);
                sunPathContrib = throughput * sunContrib;
            }

            if (!terminalEmissiveHit)
            {
                // ReSTIR DI initial sampling (roadmap P2): resample bounce-0 emissive + env direct
                // when enabled. Gated to the primary hit (bounce 0); the GI tail keeps plain NEE
                // (its reuse is P5+). Sun and SH ambient are untouched. candidateCount=1 reproduces
                // the plain-NEE estimator exactly (A/B parity anchor).
                const uint diCandidates = g_PtRestirDiCandidateCount;
                if (diCandidates > 0u && bounce == 0u)
                {
                    const float3 emissiveNee = opaqueWeight
                        * RestirDiEmissiveDirect(
                            rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                            hitNormal, shadowOrigin, diCandidates, freshDiReservoirs.emissive);
                    emissiveNeeContrib = throughput * emissiveNee;

                    const float3 envNee = opaqueWeight
                        * RestirDiEnvironmentDirect(
                            rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                            hitNormal, shadowOrigin, diCandidates, freshDiReservoirs.environment);
                    envNeeContrib = throughput * envNee;
                    freshDiRadiance = emissiveNeeContrib + envNeeContrib;
                }
                else
                {
                    // Mesh-light NEE must run in the GI tail too. Otherwise Cornell-box indirect
                    // lighting relies on random BSDF rays hitting the ceiling light, which is extreme
                    // variance and presents as frame-to-frame boiling before ReSTIR even runs.
                    const float3 emissiveNee = opaqueWeight
                        * EvaluateDirectEmissive(
                            rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                            hitNormal, shadowOrigin);
                    emissiveNeeContrib = throughput * emissiveNee;

                    const float3 envNee = opaqueWeight
                        * EvaluateDirectEnvironment(
                            rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                            hitNormal, shadowOrigin);
                    envNeeContrib = throughput * envNee;
                }
            }
        }
        if (inTail)
        {
            loTail += sunPathContrib;
            loTail += emissiveNeeContrib;
            loTail += envNeeContrib;
            if (tracingPrimaryTransmissionLayer)
            {
                const float3 receiverLightingContrib =
                    sunPathContrib + emissiveNeeContrib + envNeeContrib;
                transmissionLoTail += receiverLightingContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                if (transmissionReceiverShadedForDebug)
                    transmissionDeepBounceForDebug += receiverLightingContrib;
                else
                    transmissionReceiverForDebug += receiverLightingContrib;
#endif
            }
        }
        else
        {
            directRadiance += sunPathContrib;
#if PT_DIAGNOSTIC_PERMUTATION
            termDirectSun += sunPathContrib;
#endif
            directRadiance += emissiveNeeContrib;
#if PT_DIAGNOSTIC_PERMUTATION
            termDirectEmissive += emissiveNeeContrib;
#endif
            // Bounce-0 env NEE is part of direct (no dedicated term AOV historically).
            directRadiance += envNeeContrib;
        }

        // Real-time v2: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md). Fills
        // crevices without the v1 washout from unoccluded per-bounce SH. Reference omits this.
        // Emissive terminals get ray-free ambient display; AO on a light surface is wasted TraceRays.
        if (kPtCenterPrimaryRays && bounce == 0u && !terminalEmissiveHit)
        {
#if PT_DIAGNOSTIC_PERMUTATION
            primaryAoVis =
                TracePrimaryAmbientOcclusion(rng, shadowOrigin, hitNormalGeom, g_PtAmbientAoRayCount);
#else
            const float primaryAoVis =
                TracePrimaryAmbientOcclusion(rng, shadowOrigin, hitNormalGeom, g_PtAmbientAoRayCount);
#endif
            // Same soft-sun sample as the radiance path (G3: do not re-draw — AOV must match).
#if PT_DIAGNOSTIC_PERMUTATION
            primarySunVis = sunVis;
#endif
            const float3 ambientContrib =
                EvaluateRealTimeDiffuseAmbient(diffuseAlbedo, hitNormal, primaryAoVis);
            const float3 ambientPathContrib = throughput * ambientContrib;
            directRadiance += ambientPathContrib;
#if PT_DIAGNOSTIC_PERMUTATION
            termAmbient += ambientPathContrib;
#endif
        }
        else if (realTimePrimaryEmitterDisplay)
        {
#if PT_DIAGNOSTIC_PERMUTATION
            primaryAoVis = 1.0;
            primarySunVis = sunVis;
#endif
            const float3 ambientContrib =
                EvaluateRealTimeEmitterDisplayAmbient(diffuseAlbedo, hitNormal);
            const float3 ambientPathContrib = throughput * ambientContrib;
            directRadiance += ambientPathContrib;
#if PT_DIAGNOSTIC_PERMUTATION
            termAmbient += ambientPathContrib;
#endif

            const float3 visibleEmissive = surfaceEmissiveColor;
            const float3 visibleEmissiveContrib = throughput * visibleEmissive;
            directRadiance += visibleEmissiveContrib;
#if PT_DIAGNOSTIC_PERMUTATION
            termSurfaceEmissive += visibleEmissiveContrib;
#endif
        }
        else if (terminalEmissiveHit && bounce > 0u && opaqueWeight > 0.0)
        {
            // Real-time emitters remain terminal for cost, but their secondary-ray appearance
            // needs the same non-emissive surface response as a directly visible emitter. SH
            // diffuse plus one occluded specular-environment ray gives a white base material real
            // face shading without launching a recursive continuation or emitter/env NEE.
            const float3 ambientContrib =
                EvaluateRealTimeEmitterDisplayAmbient(diffuseAlbedo, hitNormal);
            const float nDotV = saturate(dot(hitNormal, viewDir));
            const float3 reflectedEnv = SampleEnvironment(reflect(-viewDir, hitNormal), surfaceRoughness)
                * EnvBrdfApprox(f0, surfaceRoughness, nDotV);
            const float3 reflectDir = reflect(-viewDir, hitNormal);
            const float specularVisibility = TraceVisibility(shadowOrigin, reflectDir, g_MaxTraceDistance);
            const float3 terminalSurfaceContrib = throughput * (
                ambientContrib + opaqueWeight * reflectedEnv * specularVisibility);
            if (inTail)
            {
                loTail += terminalSurfaceContrib;
                if (tracingPrimaryTransmissionLayer)
                {
                    transmissionLoTail += terminalSurfaceContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                    if (transmissionReceiverShadedForDebug)
                        transmissionDeepBounceForDebug += terminalSurfaceContrib;
                    else
                        transmissionReceiverForDebug += terminalSurfaceContrib;
#endif
                }
            }
            else
            {
                directRadiance += terminalSurfaceContrib;
            }
        }

        if (bounce == 0u)
        {
            primary.hit = true;
            primary.roughness = surfaceRoughness;
            primary.dielectricWeight = dielectricWeight;
            primary.worldPos = psrOwned
                ? PtMirrorTransformPoint(mirrorVirtualTransform, hitPos)
                : hitPos;
            primary.geomNormal = psrOwned
                ? normalize(PtMirrorTransformDirection(mirrorVirtualTransform, hitNormalGeom))
                : hitNormalGeom;
            primary.shadingNormal = psrOwned
                ? normalize(PtMirrorTransformDirection(mirrorVirtualTransform, hitNormal))
                : hitNormal;
            primary.albedo = albedo;
            primary.metallic = surfaceMetallic;
            const uint primaryMaterialId = g_GeometryLookup[payload.instanceId].materialId;
#if PT_DIAGNOSTIC_PERMUTATION
            primaryMaterialIdForDebug = primaryMaterialId;
#endif
            float resolvedPrimaryDepth = payload.primaryDepth;
            float2 resolvedPrimaryMotion = PayloadPrimaryMotion(payload);
            // S1-P2 diagnostic reference. This remains the opaque primary value even if the
            // dielectric guide below replaces the exported guide motion with virtual motion.
#if PT_DIAGNOSTIC_PERMUTATION
            opaquePrimaryMotionForDebug = resolvedPrimaryMotion;
#endif
            const float primaryPreviousLinearDepth = PayloadPrevLinearDepth(payload);
            const float nDotVPrimary = saturate(dot(hitNormalGeom, viewDir));

            float3 diffuseGuide;
            float3 specGuide;
            float3 guideNormal;
            float guideRoughness;
            ComputePtPrimaryRrMaterialGuides(
                albedo,
                hitNormal,
                surfaceRoughness,
                surfaceMetallic,
                material.transmission,
                material.indexOfRefraction,
                viewDir,
                diffuseGuide,
                specGuide,
                guideNormal,
                guideRoughness);

            // Every supported smooth optical pixel exports one complete receiver bundle.  For
            // refraction, its depth, motion, and material guides describe the refracted background
            // receiver rather than the glass polygon (devdoc/dxr/pt/transmission-rr-guides.md).
            // Never blend receiver domains: each guide bundle belongs to one selected optical lobe.
            const float guideOriginBias =
                max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotVPrimary));
            const bool smoothOptical = surfaceRoughness <= kPtDeltaSpecularRoughness;
            const bool thinPane = material.thinWalled > 0.5;
            const FirstOpticalInterface firstOpticalInterface = BuildFirstOpticalInterface(
                hitNormal, ray.Direction, material.indexOfRefraction, thinPane, false);
            // Separated optical histories export both lobes. The all-off compatibility path uses
            // the pre-experiment Fresnel-dominant receiver so the shared RR history sees the same
            // depth, motion, and material domain it did before this work.
            const bool transmissionLobeDominant =
                firstOpticalInterface.refractValid && firstOpticalInterface.fresnel <= 0.5;
            const bool reflectionLobeDominant =
                !firstOpticalInterface.refractValid || firstOpticalInterface.fresnel > 0.5;
            const bool selectTransmissionReceiver = dielectricWeight > 0.0
                && smoothOptical
                && (kPtLegacyOpticalRouting
                    ? transmissionLobeDominant
                    : firstOpticalInterface.refractValid);
            // Exact virtual geometry requires a static planar, normal-map-free, zero-diffuse delta
            // link. The feature claims opaque mirror primaries as one domain: unsupported curved,
            // glossy, mixed, or moving links retain the complete bounce-zero bundle instead of
            // silently falling back to the incompatible one-hop/direct-projection representation.
            const bool mirrorFeatureClaimsOpaqueMetal = kPtMirrorChainPsrEnabled
                && dielectricWeight <= 0.0
                && surfaceMetallic >= 0.5;
            const bool opaqueMirrorOwnedByRadiancePath = mirrorFeatureClaimsOpaqueMetal
                && IsPtExactPlanarDeltaMirror(
                    payload, albedo, surfaceRoughness, surfaceMetallic, dielectricWeight);
            const bool selectReflectionReceiver = !mirrorFeatureClaimsOpaqueMetal
                && smoothOptical
                && (kPtLegacyOpticalRouting
                    ? ((dielectricWeight > 0.0 && reflectionLobeDominant)
                        || (dielectricWeight <= 0.0 && surfaceMetallic >= 0.5))
                    : (dielectricWeight > 0.0 || surfaceMetallic >= 0.5));
            const bool opticalMaterial = dielectricWeight > 0.0 || surfaceMetallic >= 0.5;
            const bool legacyOpticalMaterial = dielectricWeight > 0.0
                || (surfaceMetallic >= 0.5 && !mirrorFeatureClaimsOpaqueMetal);
            const bool primaryOpticalMoved = PayloadInstanceMoved(payload);
            // Block 3 fallback policy: rough optical BSDFs and any moving optical interface or
            // receiver are omitted.  Previous replay still traces the current TLAS, so exporting
            // it as valid history for those paths would be knowingly incorrect.
            bool omitOpticalGuide = (dielectricWeight > 0.0
                    && !selectTransmissionReceiver && !selectReflectionReceiver)
                || (mirrorFeatureClaimsOpaqueMetal && !opaqueMirrorOwnedByRadiancePath)
                || (!opaqueMirrorOwnedByRadiancePath
                    && surfaceMetallic >= 0.5 && !smoothOptical)
                || (legacyOpticalMaterial && primaryOpticalMoved);

#if PT_DIAGNOSTIC_PERMUTATION
            if (opticalMaterial && !smoothOptical) opticalFallbackFlagsForDebug |= 1u;
            if (opticalMaterial && primaryOpticalMoved) opticalFallbackFlagsForDebug |= 2u;
#endif

#if PT_DIAGNOSTIC_PERMUTATION
            hasFirstOpticalInterfaceForDebug = dielectricWeight > 0.0 && smoothOptical;
            firstOpticalInterfaceForDebug = firstOpticalInterface;
#endif

            if (selectTransmissionReceiver)
            {
                const float shellBias = thinPane ? max(guideOriginBias, kThinShellMinExitBias) : guideOriginBias;
                const TransmissionGuideHit txGuide = TraceTransmissionGuide(
                    hitPos, firstOpticalInterface, material.indexOfRefraction, shellBias, payload.instanceId);
                bool transmissionReplayValid;
                float2 virtualMotion = ComputeTransmissionVirtualMotion(
                    pixel, txGuide, PayloadPrimaryMotion(payload), transmissionReplayValid);
                float txDepth = txGuide.depth;
                float3 txGeomNormal = txGuide.normal;
                float3 txShadingNormal = txGuide.shadingNormal;
                float3 txReceiverDirection = txGuide.refractDir;
                if (psrGlassReceiver && txGuide.valid)
                {
                    float3 virtualReceiverPosition;
                    float virtualReceiverLinearDepth;
                    if (!ProjectPtPsrVirtualReceiver(
                            mirrorVirtualTransform,
                            txGuide.backgroundWorldPos,
                            virtualReceiverPosition,
                            txDepth,
                            virtualReceiverLinearDepth))
                    {
                        transmissionReplayValid = false;
                    }
                    else
                    {
                        virtualMotion = ComputePsrOpticalReceiverMotion(
                            txGuide.backgroundWorldPos,
                            mirrorVirtualTransform,
                            transmissionReplayValid);
                        txGeomNormal = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, txGuide.normal));
                        txShadingNormal = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, txGuide.shadingNormal));
                        txReceiverDirection = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, txGuide.refractDir));
                    }
                }
#if PT_DIAGNOSTIC_PERMUTATION
                transmissionVirtualMotionForDebug = virtualMotion;
                hasTransmissionVirtualMotionForDebug = true;
                if (txGuide.receiverMoved) opticalFallbackFlagsForDebug |= 4u;
                transmissionReceiverReprojectionForDebug = DiagnoseTransmissionReceiverReprojection(
                    pixel, virtualMotion, txGuide, shellBias);
                opticalReceiverReprojectionForDebug = transmissionReceiverReprojectionForDebug;
                transmissionReplayStatusForDebug = txGuide.valid
                    ? EncodeOpticalReplayStatus(
                        true, transmissionReplayValid, transmissionReceiverReprojectionForDebug)
                    // A refracted sky has no instance receiver, but its anchored direction is a
                    // coherent supported history domain rather than a missing optical lobe.
                    : float3(0.0, 1.0, 1.0);
#endif
                if (txGuide.valid && !txGuide.receiverMoved && !primaryOpticalMoved)
                {
                    float3 txDiffuseGuide;
                    float3 txSpecGuide;
                    float3 txGuideNormal;
                    float txGuideRoughness;
                    ComputeOpticalReceiverMaterialGuides(
                        txGuide.instanceId, txGuide.primitiveIndex, txGuide.barycentrics,
                        txGeomNormal, txShadingNormal, txReceiverDirection, txGuide.triangleLod,
                        pathConeWidth, txDiffuseGuide, txSpecGuide, txGuideNormal, txGuideRoughness);
                    if (kPtLegacyOpticalRouting)
                    {
                        resolvedPrimaryDepth = txDepth;
                        resolvedPrimaryMotion = virtualMotion;
                        diffuseGuide = txDiffuseGuide;
                        specGuide = txSpecGuide;
                        guideNormal = txGuideNormal;
                        guideRoughness = txGuideRoughness;
                        specHitDistGuide = g_MaxTraceDistance;
                    }
                    else
                    {
                        g_OpticalTransmissionDepth[pixel] = txDepth;
                        g_OpticalTransmissionMotion[pixel] = float4(virtualMotion, 0.0, 1.0);
                        g_OpticalTransmissionDiffuseAlbedo[pixel] = float4(txDiffuseGuide, 1.0);
                        g_OpticalTransmissionSpecularAlbedo[pixel] = float4(txSpecGuide, 1.0);
                        g_OpticalTransmissionNormalRoughness[pixel] =
                            float4(txGuideNormal, txGuideRoughness);
                    }
#if PT_DIAGNOSTIC_PERMUTATION
                    guideReceiverIdForDebug = txGuide.instanceId + 1u;
#endif
                }
                else
                {
                    // A sky receiver is a coherent domain; do not retain pane materials or motion.
                    if (kPtLegacyOpticalRouting)
                    {
                        resolvedPrimaryDepth = 1.0;
                        resolvedPrimaryMotion = ComputeSkyAnchorMotion(txGuide.refractDir);
                        diffuseGuide = 0.5.xxx;
                        specGuide = 0.5.xxx;
                        guideNormal = float3(0.0, 0.0, 1.0);
                        guideRoughness = 1.0;
                        specHitDistGuide = g_MaxTraceDistance;
                    }
                    else
                    {
                        g_OpticalTransmissionDepth[pixel] = 1.0;
                        const float3 transmissionSkyDirection = psrGlassReceiver
                            ? normalize(PtMirrorTransformDirection(
                                mirrorVirtualTransform, txGuide.refractDir))
                            : txGuide.refractDir;
                        g_OpticalTransmissionMotion[pixel] =
                            float4(ComputeSkyAnchorMotion(transmissionSkyDirection), 0.0, 1.0);
                    }
                }
            }
            if (selectReflectionReceiver)
            {
                const ReflectionGuideHit reflectionGuide = TraceReflectionGuide(
                    hitPos, firstOpticalInterface.reflectDir, guideOriginBias);
                bool reflectionReplayValid;
                float2 reflectionMotion = ComputeReflectionVirtualMotion(
                    pixel, reflectionGuide, PayloadPrimaryMotion(payload), reflectionReplayValid);
                float reflectionDepth = reflectionGuide.depth;
                float3 reflectionGeomNormal = reflectionGuide.normal;
                float3 reflectionShadingNormal = reflectionGuide.shadingNormal;
                float3 reflectionReceiverDirection = reflectionGuide.reflectDir;
                if (psrGlassReceiver && reflectionGuide.valid)
                {
                    float3 virtualReceiverPosition;
                    float virtualReceiverLinearDepth;
                    if (!ProjectPtPsrVirtualReceiver(
                            mirrorVirtualTransform,
                            reflectionGuide.receiverWorldPos,
                            virtualReceiverPosition,
                            reflectionDepth,
                            virtualReceiverLinearDepth))
                    {
                        reflectionReplayValid = false;
                    }
                    else
                    {
                        reflectionMotion = ComputePsrOpticalReceiverMotion(
                            reflectionGuide.receiverWorldPos,
                            mirrorVirtualTransform,
                            reflectionReplayValid);
                        reflectionGeomNormal = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, reflectionGuide.normal));
                        reflectionShadingNormal = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, reflectionGuide.shadingNormal));
                        reflectionReceiverDirection = normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, reflectionGuide.reflectDir));
                    }
                }
                else if (psrGlassReceiver)
                {
                    const float3 virtualReflectionDirection = normalize(
                        PtMirrorTransformDirection(
                            mirrorVirtualTransform, reflectionGuide.reflectDir));
                    reflectionMotion = ComputeSkyAnchorMotion(virtualReflectionDirection);
                    reflectionReplayValid = mirrorVirtualTransform.valid;
                }
#if PT_DIAGNOSTIC_PERMUTATION
                if (reflectionGuide.receiverMoved) opticalFallbackFlagsForDebug |= 4u;
                reflectionReceiverReprojectionForDebug = DiagnoseReflectionReceiverReprojection(
                    pixel, reflectionMotion, reflectionGuide, guideOriginBias);
                opticalReceiverReprojectionForDebug = reflectionReceiverReprojectionForDebug;
                reflectionReplayStatusForDebug = reflectionGuide.valid
                    ? EncodeOpticalReplayStatus(
                        true, reflectionReplayValid, reflectionReceiverReprojectionForDebug)
                    : (reflectionReplayValid
                        ? float3(0.0, 1.0, 1.0)
                        : float3(1.0, 0.0, 0.0));
#endif
                if (reflectionGuide.valid && !reflectionGuide.receiverMoved
                    && !primaryOpticalMoved && reflectionReplayValid)
                {
                    resolvedPrimaryDepth = reflectionDepth;
                    resolvedPrimaryMotion = reflectionMotion;
                    ComputeOpticalReceiverMaterialGuides(
                        reflectionGuide.instanceId, reflectionGuide.primitiveIndex, reflectionGuide.barycentrics,
                        reflectionGeomNormal, reflectionShadingNormal, reflectionReceiverDirection,
                        reflectionGuide.triangleLod, pathConeWidth,
                        diffuseGuide, specGuide, guideNormal, guideRoughness);
                    specHitDistGuide = max(reflectionGuide.reflectedHitDistance, 0.05);
#if PT_DIAGNOSTIC_PERMUTATION
                    guideReceiverIdForDebug = reflectionGuide.instanceId + 1u;
#endif
                }
                else if (!reflectionGuide.valid && !primaryOpticalMoved && reflectionReplayValid)
                {
                    // A reflected environment direction is a complete virtual receiver domain.
                    // Its inverse replay motion maps that direction through the previous optical
                    // surface; neutral far-field guides prevent the glass primary from leaking in.
                    resolvedPrimaryDepth = 1.0;
                    const float3 reflectionSkyDirection = psrGlassReceiver
                        ? normalize(PtMirrorTransformDirection(
                            mirrorVirtualTransform, reflectionGuide.reflectDir))
                        : reflectionGuide.reflectDir;
                    resolvedPrimaryMotion = psrGlassReceiver
                        ? ComputeSkyAnchorMotion(reflectionSkyDirection)
                        : reflectionMotion;
                    diffuseGuide = 0.5.xxx;
                    specGuide = 0.5.xxx;
                    guideNormal = float3(0.0, 0.0, 1.0);
                    guideRoughness = 1.0;
                    specHitDistGuide = g_MaxTraceDistance;
                }
                else
                {
                    omitOpticalGuide = true;
                }
            }

            if (omitOpticalGuide)
            {
                // Explicit RR omission: neutral material, far depth, zero motion and no hit distance.
                resolvedPrimaryDepth = 1.0;
                resolvedPrimaryMotion = 0.0.xx;
                diffuseGuide = 0.5.xxx;
                specGuide = 0.5.xxx;
                guideNormal = float3(0.0, 0.0, 1.0);
                guideRoughness = 1.0;
                specHitDistGuide = g_MaxTraceDistance;
            }
            if (!psrOwned || psrGlassReceiver)
            {
                g_DiffuseAlbedoGuide[pixel] = float4(diffuseGuide, 1.0);
                g_SpecularAlbedoGuide[pixel] = float4(specGuide, 1.0);
                g_NormalRoughnessGuide[pixel] = float4(guideNormal, guideRoughness);
            }

            // PF2: retire bounce-zero output state before secondary traces. Only `primary`, which
            // ReSTIR GI reconnects after the path terminates, remains live through the loop.
            const float restirLinearViewDepth = abs(mul(g_WorldToView, float4(primary.worldPos, 1.0)).z);
            const uint surfaceFlags = 1u
                | (primary.dielectricWeight > 0.01 ? 2u : 0u)
                | (primary.roughness <= kPtDeltaSpecularRoughness ? 4u : 0u)
                | (psrOwned ? 8u : 0u);
            g_RestirSurfacePositionDepth[pixel] = float4(primary.worldPos, restirLinearViewDepth);
            g_RestirSurfaceMaterial[pixel] = uint4(
                RestirPackOctNormal(primary.geomNormal),
                RestirPackOctNormal(primary.shadingNormal),
                ((payload.instanceId + 1u) & 0x00ffffffu) | (surfaceFlags << 24u),
                (primaryMaterialId & 0xffffu) | (f32tof16(primary.roughness) << 16u));
            g_RestirSurfaceAlbedoMetallic[pixel] = float4(primary.albedo, primary.metallic);
            if (!psrOwned || psrGlassReceiver)
            {
                g_DepthOutput[pixel] = resolvedPrimaryDepth;
            }
            const float previousDepthDelta = primaryPreviousLinearDepth > 0.0
                ? primaryPreviousLinearDepth - restirLinearViewDepth
                : 0.0;
            if (!psrOwned || psrGlassReceiver)
            {
                g_MotionOutput[pixel] = float4(
                    resolvedPrimaryMotion,
                    psrGlassReceiver ? 0.0 : previousDepthDelta,
                    1.0);
                g_SpecularMotion[pixel] = resolvedPrimaryMotion;
            }
            g_Metadata[pixel] = uint2(payload.instanceId + 1u, payload.primitiveIndex);
        }

        if (kPtMirrorChainPsrEnabled && mirrorChainActive && terminalEmissiveHit)
        {
            const float3 mirrorGuideThroughput = throughputAfterFirstScatter * throughput;
            const PtRrGuideOwner emissiveOwner = BuildPtRrSurfaceGuideOwner(
                payload,
                hitPos,
                hitNormal,
                albedo,
                surfaceRoughness,
                surfaceMetallic,
                material.transmission,
                material.indexOfRefraction,
                mirrorVirtualTransform,
                mirrorGuideThroughput,
                mirrorChainLength,
                mirrorChainDistance);
            CommitPtRrGuideOwner(pixel, emissiveOwner, specHitDistGuide);
#if PT_DIAGNOSTIC_PERMUTATION
            mirrorOwnerValidForDebug = emissiveOwner.valid;
            mirrorOwnerSkyForDebug = false;
            mirrorOwnerChainLengthForDebug = emissiveOwner.chainLength;
            mirrorOwnerConfidenceForDebug = emissiveOwner.confidence;
            mirrorOwnerInstanceIdForDebug = emissiveOwner.instanceId;
            mirrorOwnerLinearDepthForDebug = emissiveOwner.linearDepth;
            mirrorOwnerMotionForDebug = emissiveOwner.motionNdc;
            mirrorNonDeltaFallbackForDebug = mirrorNonDeltaFallbackForDebug || !emissiveOwner.valid;
#endif
            mirrorChainActive = false;
        }

        if (terminalEmissiveHit)
        {
            if (pendingPrimaryTransmissionTail)
            {
                ray = pendingTransmissionRay;
                throughput = pendingTransmissionThroughput;
                pathInMedium = pendingTransmissionPathInMedium;
                mediumTint = pendingTransmissionMediumTint;
                pathConeWidth = pendingTransmissionConeWidth;
                missEnvRoughness = pendingTransmissionMissRoughness;
                addEnvOnMiss = pendingTransmissionAddEnvOnMiss;
                lastScatterPdf = kDeltaScatterPdf;
                pendingPrimaryTransmissionTail = false;
                tracingPrimaryTransmissionLayer = true;
#if PT_DIAGNOSTIC_PERMUTATION
                transmissionContinuationResumedForDebug = true;
                transmissionReceiverShadedForDebug = false;
#endif
                bounce = 1u;
                continue;
            }
            break;
        }

        if (bounce >= maxBounces)
        {
            if (kPtMirrorChainPsrEnabled && mirrorChainActive)
            {
                // The environment tail below is an integrator approximation, not an intersected
                // receiver. Do not attach precise finite guides to it.
                mirrorChainActive = false;
#if PT_DIAGNOSTIC_PERMUTATION
                mirrorBounceCapFallbackForDebug = true;
#endif
            }
            // Terminal specular tail: the mirror-direction environment, energy-weighted (a
            // hall-of-mirrors fade instead of black). The DIFFUSE tail is covered by primary-hit SH
            // in real-time, or added here in reference mode. Only fires for paths that did not escape.
            const float terminalNdotV = saturate(dot(hitNormal, viewDir));
            const float3 reflectTail =
                SampleEnvironment(reflect(-viewDir, hitNormal), TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight))
                * EnvBrdfApprox(f0, surfaceRoughness, terminalNdotV);
            const float3 transmitTail = SampleEnvironment(
                ray.Direction,
                TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight));
            float3 terminal = lerp(reflectTail, transmitTail, dielectricWeight);
            if (!kPtCenterPrimaryRays && g_EnvLightImportanceCount == 0u)
            {
                terminal += diffuseAlbedo * EvaluateDiffuseIrradianceSh(hitNormal) / kPi
                    * g_EnvironmentIntensity;
            }
            const float3 terminalContrib = throughput * terminal;
            if (inTail)
            {
                loTail += terminalContrib;
                if (tracingPrimaryTransmissionLayer)
                {
                    transmissionLoTail += terminalContrib;
#if PT_DIAGNOSTIC_PERMUTATION
                    if (transmissionReceiverShadedForDebug)
                        transmissionDeepBounceForDebug += terminalContrib;
                    else
                        transmissionReceiverForDebug += terminalContrib;
#endif
                }
            }
            else
            {
                directRadiance += terminalContrib;
            }
            if (pendingPrimaryTransmissionTail)
            {
                ray = pendingTransmissionRay;
                throughput = pendingTransmissionThroughput;
                pathInMedium = pendingTransmissionPathInMedium;
                mediumTint = pendingTransmissionMediumTint;
                pathConeWidth = pendingTransmissionConeWidth;
                missEnvRoughness = pendingTransmissionMissRoughness;
                addEnvOnMiss = pendingTransmissionAddEnvOnMiss;
                lastScatterPdf = kDeltaScatterPdf;
                pendingPrimaryTransmissionTail = false;
                tracingPrimaryTransmissionLayer = true;
#if PT_DIAGNOSTIC_PERMUTATION
                transmissionContinuationResumedForDebug = true;
                transmissionReceiverShadedForDebug = false;
#endif
                bounce = 1u;
                continue;
            }
            break;
        }

        const float3 throughputBeforeScatter = throughput;
        float3 nextDir;
        bool isSpecular = false;
        uint scatterEvent = kPtScatterEventInvalid;
        float scatterPdf = 1.0;
        const bool pathInMediumBefore = pathInMedium;
        const bool useFirstOpticalInterface = bounce == 0u
            && dielectricWeight > 0.0
            && surfaceRoughness <= kPtDeltaSpecularRoughness;
        // This record is intentionally local to bounce zero: it is the smooth radiance interface
        // consumed by the guide above, not a global Ng/Ns contract change.
        const FirstOpticalInterface firstOpticalInterface = BuildFirstOpticalInterface(
            hitNormal,
            ray.Direction,
            material.indexOfRefraction,
            material.thinWalled > 0.5,
            pathInMedium);
        uint actualOpticalEvent = kFirstOpticalEventNone;
        const bool deterministicPrimaryOpticalSplit =
            kPtDeterministicOpticalSplitEnabled
            && kPtCenterPrimaryRays
            && bounce == 0u
            && useFirstOpticalInterface
            && firstOpticalInterface.refractValid;
        if (deterministicPrimaryOpticalSplit)
        {
#if PT_DIAGNOSTIC_PERMUTATION
            deterministicSplitAcceptedForDebug = true;
#endif
            // In real-time PT the conventional estimator follows only the glass component and
            // applies dielectricWeight once.  Evaluate both delta lobes instead: F*Lr + (1-F)*Lt.
            // This removes the high-contrast red/green event switching visible at IOR 1.5+.
            const float reflectWeight = dielectricWeight * firstOpticalInterface.fresnel;
            const float transmitWeight = dielectricWeight * (1.0 - firstOpticalInterface.fresnel);
            const float primaryNdotV = saturate(dot(hitNormalGeom, viewDir));
            const float primaryOriginBias =
                max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - primaryNdotV));
            const bool primaryThinPane = material.thinWalled > 0.5;
            const bool transmitPathInMedium = primaryThinPane
                ? false
                : firstOpticalInterface.enteringMedium;
            float3 transmitOrigin = hitPos + firstOpticalInterface.refractDir * primaryOriginBias;
            if (primaryThinPane)
            {
                transmitOrigin = hitPos + firstOpticalInterface.refractDir
                    * max(primaryOriginBias, kThinShellMinExitBias);
            }
            else if (!pathInMediumBefore && transmitPathInMedium)
            {
                transmitOrigin = hitPos + firstOpticalInterface.refractDir
                    * max(primaryOriginBias, 0.02);
            }
            else if (pathInMediumBefore && !transmitPathInMedium)
            {
                transmitOrigin = hitPos + firstOpticalInterface.refractDir
                    * max(primaryOriginBias, 0.02);
            }

            pendingPrimaryTransmissionTail = transmitWeight > 0.0;
            pendingTransmissionRay.Origin = transmitOrigin;
            pendingTransmissionRay.Direction = firstOpticalInterface.refractDir;
            pendingTransmissionRay.TMin = 0.001;
            pendingTransmissionRay.TMax = g_MaxTraceDistance;
            pendingTransmissionThroughput = transmitWeight.xxx;
            pendingTransmissionPathInMedium = transmitPathInMedium;
            pendingTransmissionMediumTint = transmitPathInMedium && !pathInMediumBefore
                ? albedo
                : (pathInMediumBefore && !transmitPathInMedium ? 1.0.xxx : mediumTint);
            pendingTransmissionConeWidth = pathConeWidth;
            pendingTransmissionMissRoughness =
                TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight);
            pendingTransmissionAddEnvOnMiss = true;

            nextDir = firstOpticalInterface.reflectDir;
            isSpecular = true;
            pathInMedium = primaryThinPane ? false : pathInMediumBefore;
            scatterPdf = kDeltaScatterPdf;
            scatterEvent = kPtScatterEventOptical;
            actualOpticalEvent = kFirstOpticalEventReflect;
            throughput *= reflectWeight;
            tracingPrimaryTransmissionLayer = false;
        }
        else
        {
            SampleMaterialBounce(
                rng,
                hitNormal,
                ray.Direction,
                viewDir,
                f0,
                albedo,
                surfaceRoughness,
                surfaceMetallic,
                material.transmission,
                material.indexOfRefraction,
                material.thinWalled > 0.5,
                pathInMedium,
                useFirstOpticalInterface,
                firstOpticalInterface,
                nextDir,
                isSpecular,
                pathInMedium,
                scatterPdf,
                scatterEvent,
                actualOpticalEvent,
                throughput);
            // Bounce zero establishes optical-layer ownership. Deeper scatters belong to that
            // same tail; clearing this at the exit face of a solid dielectric discarded all
            // radiance subsequently reached by the stored transmission continuation.
            if (bounce == 0u)
            {
                tracingPrimaryTransmissionLayer =
                    !kPtLegacyOpticalRouting
                    && actualOpticalEvent == kFirstOpticalEventTransmit;
            }
        }

#if PT_DIAGNOSTIC_PERMUTATION
        // The current surface's lighting belongs to the first opaque receiver. Contributions
        // reached after its scatter are the genuinely deeper indirect-lighting bucket.
        if (tracingPrimaryTransmissionLayer && opaqueWeight > 0.0)
        {
            transmissionReceiverShadedForDebug = true;
        }
        if (bounce == 0u && useFirstOpticalInterface)
        {
            actualOpticalEventForDebug = actualOpticalEvent;
        }
#endif
        if (pathInMedium && !pathInMediumBefore && material.thinWalled < 0.5)
        {
            mediumTint = albedo;
        }
        else if (!pathInMedium && pathInMediumBefore)
        {
            mediumTint = 1.0.xxx;
        }

        lastScatterPdf = scatterPdf;

        // After the primary scatter: freeze connection throughput and start Lo_tail at xs with
        // local throughput = 1 (ReSTIR GI / G5). Shade reconnects as t1 * Lo_tail.
        if (bounce == 0u)
        {
            if (deterministicPrimaryOpticalSplit)
            {
                // Smooth dielectric primaries never enter ReSTIR GI. Keep the Fresnel weight in
                // the live path so the two independent tails add directly into loTail.
                throughputAfterFirstScatter = 1.0.xxx;
                haveInitialSample = true;
            }
            else
            {
                throughputAfterFirstScatter = throughput;
                throughput = 1.0.xxx;
            }
            inTail = true;
            if (primary.dielectricWeight > 0.01
                || primary.roughness <= kPtDeltaSpecularRoughness
                || scatterPdf >= kDeltaScatterPdf * 0.5)
            {
                sampleFlags |= kRestirSampleNoReuse;
            }
        }

        // Real-time: specular bounces add env on miss; diffuse bounces use primary-hit SH only.
        // Reference: always add the true sky. Transmitted rays always see through on miss.
        addEnvOnMiss = kPtCenterPrimaryRays ? (isSpecular || pathInMedium) : true;

        missEnvRoughness = TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight);

        if (kPtRussianRouletteEnabled && bounce >= kRussianRouletteStartBounce)
        {
            // Measure full-path throughput for the kill probability (parity with pre-G5), but only
            // divide the local Lo_tail throughput — shade still multiplies by t1.
            const float3 rrMeasure =
                inTail ? (throughputAfterFirstScatter * throughput) : throughput;
            const float rrProb = min(
                max(rrMeasure.r, max(rrMeasure.g, rrMeasure.b)),
                kRussianRouletteMaxProb);
            const float rrXi = PathRngNext(rng);
            if (rrProb <= 1e-4 || rrXi > rrProb)
            {
                if (kPtMirrorChainPsrEnabled && mirrorChainActive)
                {
                    // Roulette terminated transport before a real receiver. Keep bounce-zero guides;
                    // the killed path supplies no truthful finite owner.
                    mirrorChainActive = false;
#if PT_DIAGNOSTIC_PERMUTATION
                    mirrorBounceCapFallbackForDebug = true;
#endif
                }
                if (pendingPrimaryTransmissionTail)
                {
                    ray = pendingTransmissionRay;
                    throughput = pendingTransmissionThroughput;
                    pathInMedium = pendingTransmissionPathInMedium;
                    mediumTint = pendingTransmissionMediumTint;
                    pathConeWidth = pendingTransmissionConeWidth;
                    missEnvRoughness = pendingTransmissionMissRoughness;
                    addEnvOnMiss = pendingTransmissionAddEnvOnMiss;
                    lastScatterPdf = kDeltaScatterPdf;
                    pendingPrimaryTransmissionTail = false;
                    tracingPrimaryTransmissionLayer = true;
#if PT_DIAGNOSTIC_PERMUTATION
                    transmissionContinuationResumedForDebug = true;
                    transmissionReceiverShadedForDebug = false;
#endif
                    bounce = 1u;
                    continue;
                }
                break;
            }
            throughput /= rrProb;
        }

        const float nDotV = saturate(dot(hitNormal, viewDir));
        const float originBias = max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV));
        const bool thinPane = material.thinWalled > 0.5 && dielectricWeight > 0.0;
        ray.Origin = hitPos + nextDir * originBias;
        if (thinPane)
        {
            // Thin slab is zero-thickness; escape any physical shell (scaled-cube panes) before continuing.
            ray.Origin = hitPos + nextDir * max(originBias, kThinShellMinExitBias);
        }
        else if (dielectricWeight > 0.0 && !pathInMediumBefore && pathInMedium)
        {
            // Entering a solid volume: push past the entry interface so the refracted segment does not
            // immediately self-hit the same shell triangle (reads as frosted/milky glass).
            ray.Origin = hitPos + nextDir * max(originBias, 0.02);
        }
        else if (dielectricWeight > 0.0 && pathInMediumBefore && !pathInMedium)
        {
            ray.Origin = hitPos + nextDir * max(originBias, 0.02);
        }
        ray.Direction = nextDir;
        bounce += 1u;
    }

    // G6: clamp Lo_tail before reservoir write; composite safety clamp remains below.
    float3 loTailForStore = loTail;
    if (kPtFireflyClampEnabled
#if PT_DIAGNOSTIC_PERMUTATION
        && g_PtDebugIsolateMode != 8u
#endif
        )
    {
        loTailForStore = ClampRadiance(loTail);
    }

    // P5 M=1 reconnection. For a fresh sample, the native solid-angle proposal already represents
    // geometry and the traced primary-to-secondary segment proves visibility. Reused samples in P6
    // must retrace visibility and apply the reconnection Jacobian.
    const bool giEligible = kPtRestirGiInitialEnabled
        && !psrOwned
        && primary.hit
        && haveInitialSample
        && primary.dielectricWeight <= 0.01
        // Low-but-non-delta lobes remain in-domain. Final shading applies the RTXDI input/output
        // MIS fallback instead of drawing a binary estimator boundary at roughness 0.2.
        && primary.roughness > kPtDeltaSpecularRoughness
        && (sampleFlags & kRestirSampleNoReuse) == 0u
        && samplePdf > 0.0
        && samplePdf < kRestirGiMaxProposalPdf;
    if (!giEligible)
    {
        sampleFlags |= kRestirSampleNoReuse;
    }
    const RestirGiReservoir giReservoir = RestirGiMakeInitialReservoir(
        sampleXs,
        sampleNs,
        loTailForStore,
        samplePdf,
        pathSeed,
        sampleFlags,
        sampleInstanceId,
        samplePrimitiveIndex);
    const float3 toSecondary = sampleXs - primary.worldPos;
    const float secondaryDistance = length(toSecondary);
    const float3 secondaryDirection = secondaryDistance > 1e-5
        ? toSecondary / secondaryDistance
        : primary.shadingNormal;
    const float3 primaryF0 = lerp(0.04.xxx, primary.albedo, primary.metallic);
    const float3 currentPrimaryBsdfCos = giEligible
        ? EvaluateOpaqueBsdf(
            primary.shadingNormal,
            normalize(g_CameraPos - primary.worldPos),
            secondaryDirection,
            primaryF0,
            primary.albedo,
            primary.roughness,
            primary.metallic)
        : 0.0.xxx;
    const float3 restirGiIndirectRaw = currentPrimaryBsdfCos * loTail * giReservoir.weightSum;
    const float3 restirGiIndirect = currentPrimaryBsdfCos * giReservoir.radiance * giReservoir.weightSum;
    const float3 baselineIndirectRaw = throughputAfterFirstScatter * loTail;
    const float3 baselineIndirect = throughputAfterFirstScatter * loTailForStore;
    const float3 termIndirect = giEligible ? restirGiIndirectRaw : baselineIndirectRaw;
    const float3 shadedIndirect = giEligible ? restirGiIndirect : baselineIndirect;
    const float3 radiancePreClamp = directRadiance + termIndirect;
    float3 radiance = directRadiance + shadedIndirect;
    if (kPtFireflyClampEnabled
#if PT_DIAGNOSTIC_PERMUTATION
        && g_PtDebugIsolateMode != 8u
#endif
        )
    {
        radiance = ClampRadiance(radiance);
    }
    const float3 psrScale = psrOwned ? max(mirrorChainThroughput, 0.0.xxx) : 1.0.xxx;
    const float3 physicalRadiancePreClamp = radiancePreClamp * psrScale;
    const float3 physicalRadiance = radiance * psrScale;

    // Preserve the exact legacy full-radiance output for non-RR consumers. The RR resolve derives
    // layer 0 as (full - transmission) and reconstructs this transmission layer independently.
    const float3 transmissionRadianceRaw = throughputAfterFirstScatter * transmissionLoTail;
    const float3 layerClampScale = float3(
        radiancePreClamp.r > 1e-6 ? radiance.r / radiancePreClamp.r : 0.0,
        radiancePreClamp.g > 1e-6 ? radiance.g / radiancePreClamp.g : 0.0,
        radiancePreClamp.b > 1e-6 ? radiance.b / radiancePreClamp.b : 0.0);
    const float3 transmissionRadiance = max(transmissionRadianceRaw * layerClampScale, 0.0.xxx);

    // P5: write raw secondary radiance and an explicit initial UCW; no source-primary shading.
    g_GiReservoirCurrent[pixelIndex] = giReservoir;
    g_ReservoirCurrent[pixelIndex] = freshDiReservoirs;

#if PT_DIAGNOSTIC_PERMUTATION
    float3 displayRadiance = SelectPtDebugRadiance(
        g_PtDebugIsolateMode,
        primary.hit,
        physicalRadiance,
        physicalRadiancePreClamp,
        termDirectSun,
        termDirectEmissive,
        termSurfaceEmissive,
        termAmbient,
        termIndirect,
        primaryAoVis,
        primarySunVis,
        specHitDistGuide);

    if (primary.hit && g_PtDebugIsolateMode == 10u)
    {
        const float linearDepth = abs(mul(g_WorldToView, float4(primary.worldPos, 1.0)).z);
        const float v = saturate(linearDepth / max(g_MaxTraceDistance, 1e-4));
        displayRadiance = float3(v, v, v);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 11u)
    {
        displayRadiance = primary.geomNormal * 0.5 + 0.5;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 12u)
    {
        displayRadiance = frac(float3(0.1031, 0.11369, 0.13787) * float(primaryMaterialIdForDebug + 1u));
    }
    else if (primary.hit && g_PtDebugIsolateMode == 13u)
    {
        displayRadiance = float3(
            primary.dielectricWeight > 0.01 ? 1.0 : 0.0,
            primary.roughness <= kPtDeltaSpecularRoughness ? 1.0 : 0.0,
            0.15);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 46u)
    {
        // Display encoding only: RG = signed opaque current-minus-previous NDC motion, with
        // 0.5 neutral and +/- 0.5 covering +/- 0.125 NDC. Numeric validation reads the guide.
        displayRadiance = float3(opaquePrimaryMotionForDebug * 4.0 + 0.5, 0.0);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 47u)
    {
        displayRadiance = hasTransmissionVirtualMotionForDebug
            ? float3(transmissionVirtualMotionForDebug * 4.0 + 0.5, 0.0)
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 49u)
    {
        // Block 1 AOV: the actual smooth optical normal consumed by radiance and the guide.
        displayRadiance = hasFirstOpticalInterfaceForDebug
            ? firstOpticalInterfaceForDebug.opticalNormal * 0.5 + 0.5
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 50u)
    {
        // Event palette: red=reflection, green=transmission, blue=TIR, black=no smooth event.
        displayRadiance = actualOpticalEventForDebug == kFirstOpticalEventReflect ? float3(1.0, 0.0, 0.0)
            : actualOpticalEventForDebug == kFirstOpticalEventTransmit ? float3(0.0, 1.0, 0.0)
            : actualOpticalEventForDebug == kFirstOpticalEventTir ? float3(0.0, 0.0, 1.0)
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 51u)
    {
        displayRadiance = hasFirstOpticalInterfaceForDebug
            ? firstOpticalInterfaceForDebug.reflectDir * 0.5 + 0.5
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 52u)
    {
        displayRadiance = hasFirstOpticalInterfaceForDebug
            && firstOpticalInterfaceForDebug.refractValid
            ? firstOpticalInterfaceForDebug.refractDir * 0.5 + 0.5
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 53u)
    {
        // Stable receiver-ID hash; zero means sky/invalid guide. This is display-only and never
        // aliases g_Metadata, whose production primary-surface meaning remains unchanged.
        displayRadiance = guideReceiverIdForDebug == 0u
            ? 0.0.xxx
            : frac(float3(0.1031, 0.11369, 0.13787) * float(guideReceiverIdForDebug));
    }
    else if (primary.hit && g_PtDebugIsolateMode == 54u)
    {
        // Fallback policy AOV: R=unsupported rough, G=moving optical primary, B=moving receiver.
        displayRadiance = float3(
            (opticalFallbackFlagsForDebug & 1u) != 0u ? 1.0 : 0.0,
            (opticalFallbackFlagsForDebug & 2u) != 0u ? 1.0 : 0.0,
            (opticalFallbackFlagsForDebug & 4u) != 0u ? 1.0 : 0.0);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 55u)
    {
        // R: receiver position residual (black is exact; red saturates at 12.5% receiver range).
        // G: receiver instance agrees; B: previous optical path at the exported history pixel exists.
        displayRadiance = opticalReceiverReprojectionForDebug;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 56u)
    {
        displayRadiance = hasFirstOpticalInterfaceForDebug
            ? float3(
                1.0,
                firstOpticalInterfaceForDebug.fresnel,
                firstOpticalInterfaceForDebug.refractValid
                    ? 1.0 - firstOpticalInterfaceForDebug.fresnel
                    : 0.0)
            : 0.0.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 57u)
    {
        displayRadiance = reflectionReceiverReprojectionForDebug;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 58u)
    {
        displayRadiance = transmissionReceiverReprojectionForDebug;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 59u)
    {
        displayRadiance = reflectionReplayStatusForDebug;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 60u)
    {
        displayRadiance = transmissionReplayStatusForDebug;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 61u)
    {
        // R: deterministic split accepted; G: stored transmission continuation resumed;
        // B: that continuation accumulated finite, nonzero radiance into its owned tail.
        displayRadiance = float3(
            deterministicSplitAcceptedForDebug ? 1.0 : 0.0,
            transmissionContinuationResumedForDebug ? 1.0 : 0.0,
            all(isfinite(transmissionLoTail))
                && max(transmissionLoTail.r, max(transmissionLoTail.g, transmissionLoTail.b)) > 1e-6
                    ? 1.0
                    : 0.0);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 62u)
    {
        displayRadiance = max(
            transmissionEnvironmentForDebug * throughputAfterFirstScatter * layerClampScale,
            0.0.xxx);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 63u)
    {
        displayRadiance = max(
            transmissionReceiverForDebug * throughputAfterFirstScatter * layerClampScale,
            0.0.xxx);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 64u)
    {
        displayRadiance = max(
            transmissionDeepBounceForDebug * throughputAfterFirstScatter * layerClampScale,
            0.0.xxx);
    }
    else if (primary.hit && g_PtDebugIsolateMode == 65u)
    {
        // Owner palette: gray=primary; red/green/blue=delta receiver length 1/2/3+;
        // cyan=sky; yellow=bounce-cap/RR terminal; magenta=non-delta/unsupported fallback.
        if (mirrorOwnerValidForDebug)
        {
            displayRadiance = mirrorOwnerSkyForDebug
                ? float3(0.0, 1.0, 1.0)
                : (mirrorOwnerChainLengthForDebug == 1u
                    ? float3(1.0, 0.0, 0.0)
                    : (mirrorOwnerChainLengthForDebug == 2u
                        ? float3(0.0, 1.0, 0.0)
                        : float3(0.0, 0.0, 1.0)));
        }
        else if (mirrorBounceCapFallbackForDebug)
        {
            displayRadiance = float3(1.0, 1.0, 0.0);
        }
        else if (mirrorNonDeltaFallbackForDebug)
        {
            displayRadiance = float3(1.0, 0.0, 1.0);
        }
        else
        {
            displayRadiance = 0.15.xxx;
        }
    }
    else if (primary.hit && g_PtDebugIsolateMode == 66u)
    {
        const uint diagnosticChainLength = mirrorOwnerValidForDebug
            ? mirrorOwnerChainLengthForDebug
            : mirrorChainLength;
        displayRadiance = saturate(float(diagnosticChainLength) / 8.0).xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 67u)
    {
        const float confidence = mirrorOwnerValidForDebug
            ? mirrorOwnerConfidenceForDebug
            : mirrorGlossyConfidenceForDebug;
        displayRadiance = saturate(confidence).xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 68u)
    {
        const uint receiverId = mirrorOwnerValidForDebug && !mirrorOwnerSkyForDebug
            ? mirrorOwnerInstanceIdForDebug + 1u
            : 0u;
        displayRadiance = receiverId == 0u
            ? 0.0.xxx
            : frac(float3(0.1031, 0.11369, 0.13787) * float(receiverId));
    }
    else if (primary.hit && g_PtDebugIsolateMode == 69u)
    {
        const float normalizedDepth = mirrorOwnerValidForDebug
            ? saturate(mirrorOwnerLinearDepthForDebug / max(g_MaxTraceDistance, 1e-4))
            : 0.0;
        displayRadiance = normalizedDepth.xxx;
    }
    else if (primary.hit && g_PtDebugIsolateMode == 70u)
    {
        displayRadiance = mirrorOwnerValidForDebug
            ? float3(mirrorOwnerMotionForDebug * 4.0 + 0.5, 1.0)
            : 0.0.xxx;
    }
    else if (g_PtDebugIsolateMode == 71u)
    {
        displayRadiance = psrTerminalReason == kPtPsrTerminalReceiver ? float3(0.0, 1.0, 0.0)
            : psrTerminalReason == kPtPsrTerminalEnvironmentEscape ? float3(0.0, 1.0, 1.0)
            : psrTerminalReason == kPtPsrTerminalSubpixelTail ? float3(0.0, 0.25, 1.0)
            : psrTerminalReason == kPtPsrTerminalHardCapSignificant ? float3(1.0, 0.0, 0.0)
            : psrTerminalReason == kPtPsrTerminalIneligibleLinkFallback ? float3(1.0, 0.0, 1.0)
            : psrTerminalReason == kPtPsrTerminalInvalidProjectionFallback ? float3(1.0, 0.5, 0.0)
            : 0.15.xxx;
    }
    else if (g_PtDebugIsolateMode == 72u)
    {
        displayRadiance = saturate(mirrorProjectedSpanPx / 16.0).xxx;
    }
    else if (g_PtDebugIsolateMode == 73u)
    {
        displayRadiance = psrOwned ? saturate(mirrorChainThroughput) : 1.0.xxx;
    }
    else if (g_PtDebugIsolateMode == 74u)
    {
        displayRadiance = psrOwned ? max(radiance, 0.0.xxx) : physicalRadiance;
    }
#else
    const float3 displayRadiance = physicalRadiance;
#endif

    // P3 base signal excludes only fresh ReSTIR DI. Temporal shading adds its reevaluated
    // emissive+environment reservoirs back without subtracting from the displayed fp16 output.
    // Temporal DI/GI starts from a subtraction-free base. Eligible P5 GI is reconstructed from
    // g_GiReservoirCurrent in the reuse pass; ineligible pixels retain their exact baseline tail.
    float3 restirBaseRadiance = directRadiance - freshDiRadiance
        + (giEligible ? 0.0.xxx : shadedIndirect);
    if (kPtFireflyClampEnabled)
    {
        restirBaseRadiance = ClampRadiance(restirBaseRadiance);
    }
    restirBaseRadiance *= psrScale;
    g_DirectOutput[pixel] = float4(restirBaseRadiance, 0.0);
    g_Output[pixel] = float4(displayRadiance, specHitDistGuide);
    if (!kPtLegacyOpticalRouting)
    {
        g_OpticalTransmissionOutput[pixel] = float4(transmissionRadiance, g_MaxTraceDistance);
    }
}

[shader("miss")]
void PathTracerMiss(inout Payload payload)
{
    if (payload.hit == kPayloadFlagVisibility)
    {
        payload.hit = 0;
        return;
    }

    payload.hit = 0;
    payload.hitDistance = g_MaxTraceDistance;
}

// Per-triangle ray-cone LOD constant (RTG ch. 20): 0.5·log2(albedo texel area / world area).
// Needs ObjectToWorld3x4, so it must run in the closest-hit; the raygen adds the per-path terms.
float ComputeTriangleAlbedoLodConstant(uint instanceId, uint primitiveIndex)
{
    const MaterialEntry material = LoadMaterialForInstance(instanceId);
    if (material.albedoTexIndex == 0xFFFFFFFFu || material.albedoUvOffsetFloats == 0xFFFFFFFFu)
    {
        return 0.0;
    }

    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];

    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3 w0 = mul(objectToWorld, float4(LoadObjectPosition(geo, i0), 1.0)).xyz;
    const float3 w1 = mul(objectToWorld, float4(LoadObjectPosition(geo, i1), 1.0)).xyz;
    const float3 w2 = mul(objectToWorld, float4(LoadObjectPosition(geo, i2), 1.0)).xyz;
    const float worldArea = 0.5 * length(cross(w1 - w0, w2 - w0));

    const float2 uv0 = LoadObjectUv(geo, i0, material.albedoUvOffsetFloats);
    const float2 uv1 = LoadObjectUv(geo, i1, material.albedoUvOffsetFloats);
    const float2 uv2 = LoadObjectUv(geo, i2, material.albedoUvOffsetFloats);
    const float2 e1 = uv1 - uv0;
    const float2 e2 = uv2 - uv0;
    const float uvArea = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);

    uint texWidth;
    uint texHeight;
    g_BindlessTextures[NonUniformResourceIndex(material.albedoTexIndex)]
        .GetDimensions(texWidth, texHeight);
    const float texelArea = uvArea * float(texWidth) * float(texHeight);

    if (worldArea <= 1e-12 || texelArea <= 1e-12)
    {
        return 0.0;
    }

    return 0.5 * log2(texelArea / worldArea);
}

// Exact unfolding needs a real plane, not a tangent-plane approximation of a curved or
// normal-mapped surface. Requiring every vertex normal to agree with the triangle normal rejects
// smoothly shaded spheres even when the interpolated normal happens to align at one barycentric.
bool TriangleSupportsPlanarMirrorUnfolding(
    uint instanceId,
    uint primitiveIndex,
    float3 triangleNormal)
{
    const MaterialEntry material = LoadMaterialForInstance(instanceId);
    if (material.normalTexIndex != 0xFFFFFFFFu)
    {
        return false;
    }

    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    if (geo.vertexStrideFloats < 6u)
    {
        return true;
    }

    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint indices[3] = {
        g_SceneIndices[indexBase + 0u],
        g_SceneIndices[indexBase + 1u],
        g_SceneIndices[indexBase + 2u] };
    const float3x4 objectToWorld = ObjectToWorld3x4();
    [unroll]
    for (uint vertex = 0u; vertex < 3u; ++vertex)
    {
        const float3 worldNormalUnnormalized = mul(
            (float3x3)objectToWorld,
            LoadObjectNormal(geo, indices[vertex]));
        const float worldNormalLengthSquared = dot(worldNormalUnnormalized, worldNormalUnnormalized);
        if (!isfinite(worldNormalLengthSquared) || worldNormalLengthSquared <= 1e-12)
        {
            return false;
        }
        const float3 worldNormal = worldNormalUnnormalized * rsqrt(worldNormalLengthSquared);
        if (abs(dot(worldNormal, triangleNormal)) < 0.99999)
        {
            return false;
        }
    }
    return true;
}

[shader("closesthit")]
void PathTracerClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    if (payload.hit == kPayloadFlagVisibility)
    {
        return;
    }

    const uint request = payload.hit;
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();

    const bool hitBackFace = HitKind() == kHitKindTriangleBackFace;
    // Glass-shadow probes need smooth shading normals (not face geom): spheres/lenses get
    // blotchy Fresnel + refraction if each triangle uses a flat geometric normal.
    float3 triangleNormal = ComputeWorldGeometricNormal(geo, primitiveIndex);
    float3 hitNormal = ComputeWorldShadingNormal(geo, primitiveIndex, attribs.barycentrics);
    if (hitBackFace)
    {
        triangleNormal = -triangleNormal;
        hitNormal = -hitNormal;
    }
    if (dot(triangleNormal, rayDir) > 0.0)
    {
        triangleNormal = -triangleNormal;
    }
    if (dot(hitNormal, rayDir) > 0.0)
    {
        hitNormal = -hitNormal;
    }

    // Compute everything into locals first, then pack once — avoids read-modify-write coupling on
    // the shared lodPrevDepthHalf dword and keeps the packing centralized.
    float3 shadingNormal = hitNormal;
    float2 barycentrics = 0.0.xx;
    float triangleLod = 0.0;
    bool planarSurface = false;

    // G7/P2: skip primary-only / shading work unless the raygen asked for it. Shadow and
    // transmission-visibility segments only need geometric normal + instance + distance.
    if ((request & kPayloadReqShadingData) != 0u)
    {
        shadingNormal = ApplyWorldNormalMap(
            instanceId, primitiveIndex, attribs.barycentrics, hitNormal, -rayDir, 0.0);
        barycentrics = attribs.barycentrics;
        triangleLod = ComputeTriangleAlbedoLodConstant(instanceId, primitiveIndex);
        if (kPtMirrorChainPsrEnabled
            && (request & kPayloadReqPrimarySurface) != 0u)
        {
            planarSurface = TriangleSupportsPlanarMirrorUnfolding(
                    instanceId, primitiveIndex, triangleNormal)
                && abs(dot(hitNormal, triangleNormal)) >= 0.99999
                && abs(dot(shadingNormal, triangleNormal)) >= 0.99999;
        }
    }

    float2 primaryMotion = 0.0.xx;
    float primaryDepth = 1.0;
    float primaryPrevLinearDepth = 0.0;
    bool instanceMoved = false;
    if ((request & kPayloadReqPrimarySurface) != 0u)
    {
        ComputeVertexInterpolatedPrimarySurface(
            primaryMotion,
            primaryDepth,
            primaryPrevLinearDepth,
            instanceId,
            primitiveIndex,
            attribs.barycentrics);
        instanceMoved = CurrentInstanceTransformMoved(instanceId);
    }

    payload.hit = 1u
        | (hitBackFace ? kPayloadHitBackFace : 0u)
        | (instanceMoved ? kPayloadHitMovingInstance : 0u)
        | (planarSurface ? kPayloadHitPlanarSurface : 0u);
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    payload.hitDistance = hitT;
    payload.normalOct = PtPackOctNormal(hitNormal);
    payload.shadingNormalOct = PtPackOctNormal(shadingNormal);
    payload.barycentricsHalf = RestirPackHalf2(barycentrics);
    payload.lodPrevDepthHalf =
        (f32tof16(triangleLod) & 0xffffu) | (f32tof16(primaryPrevLinearDepth) << 16);
    payload.primaryMotionHalf = RestirPackHalf2(primaryMotion);
    payload.primaryDepth = primaryDepth;
}
