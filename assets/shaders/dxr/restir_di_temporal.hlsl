// ReSTIR DI temporal reuse (production roadmap P3).
// Fresh and previous typed light samples are reevaluated at the current primary receiver.

#include "restir_di.hlsli"
#include "restir_pack.hlsli"

cbuffer RestirTemporalConstants : register(b0)
{
    uint2 g_OutputSize;
    uint g_HistoryValid;
    uint g_FrameIndex;
    float4x4 g_InvViewProj;
    float3 g_CameraPos;
    float g_MaxTraceDistance;
    float3 g_PrevCameraPos;
    float g_SpatialFilterStrength;
    uint g_ShadeOutput;
    uint g_SpatialSampleCount;
    float g_SpatialRadius;
    uint g_SpatialIteration;
    uint g_EmissiveLightCount;
    float g_EmissiveLightPickWeightSum;
    uint g_EnvImportanceCount;
    uint g_EnvCdfWidth;
    uint g_EnvCdfHeight;
    float g_EnvironmentIntensity;
    float g_EnvDirectLuminanceClamp;
    float g_AnalyticSunActive;
    float3 g_SunDirection;
    float g_SunAngularTanRadius;
    uint g_DebugMode;
    uint g_EnableDiTemporal;
    uint g_EnableGiTemporal;
    float g_EnvironmentRotationY;
};

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float4> g_PrevSurfacePositionDepth : register(t1);
Texture2D<uint4> g_PrevSurfaceMaterial : register(t2);
Texture2D<float4> g_CurrSurfacePositionDepth : register(t3);
Texture2D<uint4> g_CurrSurfaceMaterial : register(t4);
Texture2D<float4> g_Motion : register(t5);
Texture2D<float4> g_BaseRadiance : register(t6);
Texture2D<float4> g_CurrAlbedoMetallic : register(t7);
Texture2D<float4> g_PrevAlbedoMetallic : register(t8);

struct EmissiveLightEntry
{
    float3 emissive;
    float pickWeight;
    uint instanceId;
    uint triangleOffset;
    uint triangleCount;
    float surfaceArea;
};
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
StructuredBuffer<EmissiveLightEntry> g_EmissiveLights : register(t9);
StructuredBuffer<EmissiveTriangleEntry> g_EmissiveTriangles : register(t10);
StructuredBuffer<float> g_EnvCdf : register(t11);
Texture2D<float4> g_EnvMap : register(t12);
SamplerState g_LinearClampSampler : register(s0);

RWStructuredBuffer<RestirDiReservoirSet> g_ReservoirCurrent : register(u0);
RWStructuredBuffer<RestirDiReservoirSet> g_ReservoirPrev : register(u1);
RWStructuredBuffer<RestirGiReservoir> g_GiReservoirCurrent : register(u2);
RWTexture2D<float4> g_Output : register(u3);
RWStructuredBuffer<RestirGiReservoir> g_GiReservoirPrev : register(u4);

struct Payload { uint hit; };

float Hash(uint2 pixel, uint salt)
{
    uint n = pixel.x * 1664525u + pixel.y * 1013904223u
        + g_FrameIndex * 747796405u + salt * 2891336453u;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n ^= n >> 16u;
    return float(n & 0x00ffffffu) / float(0x01000000u);
}

float3 SignedLogRatioColor(float ratio)
{
    const float signedMagnitude = clamp(log2(max(ratio, 1.0e-6)) / 4.0, -1.0, 1.0);
    const float3 neutral = 0.5.xxx;
    return signedMagnitude >= 0.0
        ? lerp(neutral, float3(1.0, 0.18, 0.04), signedMagnitude)
        : lerp(neutral, float3(0.04, 0.25, 1.0), -signedMagnitude);
}

float2 PixelToNdc(uint2 p)
{
    float2 uv = (float2(p) + 0.5) / float2(g_OutputSize);
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

bool SurfaceCompatible(float4 aPos, uint4 aMat, float4 bPos, uint4 bMat)
{
    const uint af = aMat.z >> 24u;
    const uint bf = bMat.z >> 24u;
    if ((af & 1u) == 0u || (bf & 1u) == 0u || (af & 6u) != 0u || (bf & 6u) != 0u)
        return false; // invalid, transmission, and delta surfaces always use fresh DI
    if (aPos.w <= 0.0 || bPos.w <= 0.0)
        return false;
    // Linear view depth is camera-relative: a forward/back camera translation changes it even for
    // the exact same static world-space point. Temporal identity is instead same instance/material
    // plus a small plane/tangent footprint around the motion-reprojected sample.
    if ((aMat.z & 0x00ffffffu) != (bMat.z & 0x00ffffffu))
        return false;
    const float3 aGeomN = RestirUnpackOctNormal(aMat.x);
    const float3 bGeomN = RestirUnpackOctNormal(bMat.x);
    if (dot(aGeomN, bGeomN) < 0.9
        || dot(RestirUnpackOctNormal(aMat.y), RestirUnpackOctNormal(bMat.y)) < 0.9)
        return false;
    if ((aMat.w & 0xffffu) != (bMat.w & 0xffffu))
        return false;
    if (abs(f16tof32(aMat.w >> 16u) - f16tof32(bMat.w >> 16u)) > 0.1)
        return false;

    const float3 worldDelta = bPos.xyz - aPos.xyz;
    const float depthScale = max(max(aPos.w, bPos.w), 1.0);
    const float planeTolerance = max(0.005, 0.0025 * depthScale);
    if (abs(dot(worldDelta, aGeomN)) > planeTolerance
        || abs(dot(worldDelta, bGeomN)) > planeTolerance)
        return false;
    const float3 tangentDelta = worldDelta - aGeomN * dot(worldDelta, aGeomN);
    return length(tangentDelta) <= max(0.02, 0.01 * depthScale);
}

// P6-only expected-depth validation. Motion Z carries
// previousLinearDepth-currentLinearDepth for the exact primary hit, matching RTXDI's temporal
// surface contract without comparing camera-relative current/previous depths directly.
bool GiPrimaryHistoryCompatible(float4 currentPos, float4 previousPos, float depthMotion)
{
    const float expectedPreviousDepth = currentPos.w + depthMotion;
    if (expectedPreviousDepth <= 0.0 || previousPos.w <= 0.0)
        return false;
    if (abs(previousPos.w - expectedPreviousDepth)
        > 0.02 * max(expectedPreviousDepth, 1e-3))
        return false;
    return true;
}

bool SpatialSurfaceCompatible(float4 centerPos, uint4 centerMat, float4 neighborPos, uint4 neighborMat)
{
    const uint centerFlags = centerMat.z >> 24u;
    const uint neighborFlags = neighborMat.z >> 24u;
    if ((centerFlags & 1u) == 0u || (neighborFlags & 1u) == 0u
        || (centerFlags & 6u) != 0u || (neighborFlags & 6u) != 0u)
    {
        return false;
    }
    // RTXDI defines the threshold relative to the canonical/current surface's linear depth.
    if (abs(centerPos.w - neighborPos.w)
        > kRestirDiSpatialDepthThreshold * max(abs(centerPos.w), 1e-3))
    {
        return false;
    }
    if (dot(RestirUnpackOctNormal(centerMat.y), RestirUnpackOctNormal(neighborMat.y))
        < kRestirDiSpatialNormalThreshold)
    {
        return false;
    }
    if (dot(RestirUnpackOctNormal(centerMat.x), RestirUnpackOctNormal(neighborMat.x))
        < kRestirDiSpatialNormalThreshold)
    {
        return false;
    }
    if ((centerMat.w & 0xffffu) != (neighborMat.w & 0xffffu))
    {
        return false;
    }
    return abs(f16tof32(centerMat.w >> 16u) - f16tof32(neighborMat.w >> 16u)) <= 0.1;
}

float GgxD(float noH, float alpha)
{
    float a2 = alpha * alpha;
    float d = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-9);
}
float SmithG1(float noX, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0 * noX / max(noX + sqrt(a2 + (1.0 - a2) * noX * noX), 1e-9);
}
float SmithG2HeightCorrelated(float noV, float noL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = noL * sqrt(a2 + (1.0 - a2) * noV * noV);
    float lambdaL = noV * sqrt(a2 + (1.0 - a2) * noL * noL);
    return 2.0 * noV * noL / max(lambdaV + lambdaL, 1e-9);
}
float3 Fresnel(float cosTheta, float3 f0)
{
    return f0 + (1.0.xxx - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}
float3 EvaluateBsdfCos(float3 n, float3 v, float3 l, float3 albedo, float metallic, float roughness)
{
    float noV = saturate(dot(n, v));
    float noL = saturate(dot(n, l));
    if (noL <= 0.0) return 0.0.xxx;
    float3 h = normalize(v + l);
    float noH = saturate(dot(n, h));
    float voH = saturate(dot(v, h));
    float alpha = max(min(max(roughness, 1e-4), 0.99) * min(max(roughness, 1e-4), 0.99), 1e-3);
    float3 f0 = lerp(0.04.xxx, albedo, metallic);
    float3 F = Fresnel(voH, f0);
    float3 fresnelNoV = Fresnel(noV, f0);
    float3 specCos = F * SmithG2HeightCorrelated(noV, noL, alpha) * GgxD(noH, alpha)
        / max(4.0 * noV, 1e-4);
    float3 diffCos = (1.0.xxx - fresnelNoV) * albedo * (1.0 - saturate(metallic))
        * (noL / 3.14159265);
    return specCos + diffCos;
}
float BsdfPdf(float3 n, float3 v, float3 l, float3 albedo, float metallic, float roughness)
{
    float noV = saturate(dot(n, v));
    float noL = saturate(dot(n, l));
    if (noV <= 0.0 || noL <= 0.0) return 0.0;
    float3 f0 = lerp(0.04.xxx, albedo, metallic);
    float lumSpec = dot(Fresnel(noV, f0), float3(0.2126, 0.7152, 0.0722));
    float lumDiff = dot(albedo * (1.0 - saturate(metallic)), float3(0.2126, 0.7152, 0.0722));
    float pSpec = lumSpec / max(lumSpec + lumDiff, 1e-4);
    pSpec = lerp(pSpec, 1.0, saturate(metallic));
    pSpec = clamp(pSpec, 0.1, 0.9);
    float3 h = normalize(v + l);
    float noH = saturate(dot(n, h));
    float alpha = max(min(max(roughness, 1e-4), 0.99) * min(max(roughness, 1e-4), 0.99), 1e-3);
    float pdfSpec = SmithG1(noV, alpha) * GgxD(noH, alpha) / max(4.0 * noV, 1e-4);
    return pSpec * pdfSpec + (1.0 - pSpec) * noL / 3.14159265;
}
float Balance(float a, float b) { return a / max(a + b, 1e-8); }

float3 EquirectUvToDirection(float2 uv)
{
    float phi = (uv.x - 0.5) * 6.2831853;
    float y = sin(3.14159265 * (uv.y - 0.5));
    float h = sqrt(max(1.0 - y * y, 0.0));
    return normalize(float3(cos(phi) * h, y, sin(phi) * h));
}

float3 RotateEnvironmentY(float3 direction, float angle)
{
    float c = cos(angle); float s = sin(angle);
    return float3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}
float EnvPdf(float2 uv)
{
    if (g_EnvImportanceCount == 0u) return 0.0;
    uint ix = min(uint(saturate(uv.x) * float(g_EnvCdfWidth)), g_EnvCdfWidth - 1u);
    uint iy = min(uint(saturate(uv.y) * float(g_EnvCdfHeight)), g_EnvCdfHeight - 1u);
    uint i = iy * g_EnvCdfWidth + ix;
    float probability = g_EnvCdf[i + 1u] - g_EnvCdf[i];
    float v = (float(iy) + 0.5) / float(g_EnvCdfHeight);
    float omega = (6.2831853 / float(g_EnvCdfWidth)) * (3.14159265 / float(g_EnvCdfHeight))
        * max(cos(3.14159265 * (v - 0.5)), 1e-6);
    return probability / max(omega, 1e-8);
}
float3 EnvRadiance(float2 uv)
{
    if (g_AnalyticSunActive > 0.5)
    {
        float cosBoundary = rsqrt(1.0 + max(g_SunAngularTanRadius, 1e-6)
            * max(g_SunAngularTanRadius, 1e-6));
        if (dot(RotateEnvironmentY(EquirectUvToDirection(uv), -g_EnvironmentRotationY), normalize(g_SunDirection)) >= cosBoundary)
            return 0.0.xxx;
    }
    float3 r = g_EnvMap.SampleLevel(g_LinearClampSampler, uv, 0.0).rgb * g_EnvironmentIntensity;
    float lum = dot(r, float3(0.2126, 0.7152, 0.0722));
    if (g_AnalyticSunActive > 0.5 && g_EnvDirectLuminanceClamp > 0.0)
        r *= min(1.0, g_EnvDirectLuminanceClamp / max(lum, 1e-6));
    return r;
}

void SampleTriangle(float3 a, float3 b, float3 c, float2 xi, out float3 p)
{
    float su = sqrt(saturate(xi.x));
    p = (1.0 - su) * a + su * xi.y * b + su * (1.0 - xi.y) * c;
}

bool EvaluateSample(
    RestirDiLightSample sample, float3 receiver, float3 n, float3 v,
    float3 albedo, float metallic, float roughness,
    out float3 contribution, out float3 wi, out float distance)
{
    contribution = 0.0.xxx; wi = float3(0, 1, 0); distance = 0.0;
    float proposal = 0.0;
    if (sample.sampleType == kRestirDiSampleEmissive)
    {
        if (sample.index0 >= g_EmissiveLightCount) return false;
        EmissiveLightEntry light = g_EmissiveLights[sample.index0];
        EmissiveTriangleEntry tri = g_EmissiveTriangles[sample.index1];
        float3 lightPoint; SampleTriangle(tri.v0, tri.v1, tri.v2, sample.uv, lightPoint);
        float3 d = lightPoint - receiver; float d2 = max(dot(d, d), 1e-8);
        distance = sqrt(d2); wi = d / distance;
        float cosE = saturate(dot(tri.faceNormal, -wi));
        if (dot(n, wi) <= 0.0 || cosE <= 0.0) return false;
        float pickPdf = tri.pickWeight / max(g_EmissiveLightPickWeightSum, 1e-8);
        proposal = pickPdf * d2 / max(tri.triangleArea * cosE, 1e-8);
        float3 bsdf = EvaluateBsdfCos(n, v, wi, albedo, metallic, roughness);
        contribution = bsdf * light.emissive * Balance(proposal, BsdfPdf(n, v, wi, albedo, metallic, roughness));
        distance = max(distance - 0.001, 0.001);
    }
    else if (sample.sampleType == kRestirDiSampleEnvironment)
    {
        wi = RotateEnvironmentY(EquirectUvToDirection(sample.uv), -g_EnvironmentRotationY); distance = g_MaxTraceDistance;
        if (dot(n, wi) <= 0.0) return false;
        proposal = EnvPdf(sample.uv);
        float3 bsdf = EvaluateBsdfCos(n, v, wi, albedo, metallic, roughness);
        contribution = bsdf * EnvRadiance(sample.uv)
            * Balance(proposal, BsdfPdf(n, v, wi, albedo, metallic, roughness));
    }
    return proposal > 0.0 && RestirDiTargetLuminance(contribution) > 0.0;
}

float Visibility(float3 receiver, float3 geomN, float3 wi, float distance)
{
    (void)geomN;
    RayDesc ray;
    ray.Origin = receiver;
    ray.Direction = wi;
    ray.TMin = 0.001;
    ray.TMax = max(distance, 0.0011);
    Payload p; p.hit = 2u;
    TraceRay(g_SceneTlas,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
        0xff, 0, 0, 0, ray, p);
    return p.hit == 0u ? 1.0 : 0.0;
}

// Shadowed RIS target: the unshadowed contribution with the sample's visibility folded in (one
// shadow ray). Keeping visibility OUT of the target lets an occluded high-target sample persist in
// the reservoir and inflate the UCW of whatever visible sample wins next, a recursive brightening
// bias proven in tests/restir_temporal_chain_test.cpp (+1.5%) and observed as the constant P6/DI
// hard line. Folding visibility into the target makes temporal reuse unbiased (verified in sim).
float EvaluateShadowedTarget(
    RestirDiLightSample sample, float3 receiver, float3 n, float3 v,
    float3 albedo, float metallic, float roughness)
{
    float3 f, wi; float dist;
    if (!EvaluateSample(sample, receiver, n, v, albedo, metallic, roughness, f, wi, dist))
        return 0.0;
    return RestirDiTargetLuminance(f) * Visibility(receiver, 0.0.xxx, wi, dist);
}

RestirDiTemporalReservoir ResampleDomain(
    RestirDiTemporalReservoir fresh, RestirDiTemporalReservoir previous,
    bool historyAccepted, uint2 pixel, uint salt,
    float3 receiver, float3 n, float3 v, float3 albedo, float metallic, float roughness,
    float3 previousReceiver, float3 previousGeomN, float3 previousN, float3 previousV,
    float3 previousAlbedo, float previousMetallic, float previousRoughness,
    out bool usedHistory)
{
    RestirDiTemporalReservoir outR = RestirDiTemporalInit();
    const float freshTarget = EvaluateShadowedTarget(
        fresh.sample, receiver, n, v, albedo, metallic, roughness);
    const uint mBeforeFresh = outR.M;
    bool selectedPrevious = false;
    RestirDiTemporalCombine(outR, fresh, freshTarget, Hash(pixel, salt));
    float freshM = float(outR.M - mBeforeFresh);
    float previousM = 0.0;
    usedHistory = false;
    if (historyAccepted && previous.M > 0u && previous.age < kRestirDiTemporalAgeCap)
    {
        const float previousTargetAtReceiver = EvaluateShadowedTarget(
            previous.sample, receiver, n, v, albedo, metallic, roughness);
        previous.age = min(previous.age + 1u, kRestirDiTemporalAgeCap);
        const uint mBeforePrevious = outR.M;
        selectedPrevious = RestirDiTemporalCombine(
            outR, previous, previousTargetAtReceiver, Hash(pixel, salt + 1u));
        previousM = float(outR.M - mBeforePrevious);
        usedHistory = previousM > 0.0;
    }
    if (outR.M > kRestirDiTemporalMCap)
    {
        const float capScale = float(kRestirDiTemporalMCap) / float(outR.M);
        outR.wSum *= capScale;
        freshM *= capScale;
        previousM *= capScale;
        outR.M = kRestirDiTemporalMCap;
    }

    // RTXDI ray-traced bias correction. Reevaluate the selected output sample's SHADOWED target at
    // every source receiver, then normalize by the source mixture that could have selected it. The
    // source-domain visibility ray is required for the fully unbiased mode (the cheaper target-only
    // BASIC mode is what produced the recursive brightening bias).
    const float selectedPreviousTarget = previousM > 0.0
        ? EvaluateShadowedTarget(
            outR.sample, previousReceiver, previousN, previousV,
            previousAlbedo, previousMetallic, previousRoughness)
        : 0.0;
    const float selectedCurrentTarget = outR.targetPdf;
    const float pi = selectedPrevious ? selectedPreviousTarget : selectedCurrentTarget;
    const float piSum = selectedCurrentTarget * freshM + selectedPreviousTarget * previousM;
    const float denominator = selectedCurrentTarget * piSum;
    outR.W = denominator > 0.0 && isfinite(denominator) && isfinite(pi)
        ? outR.wSum * pi / denominator
        : 0.0;
    return outR;
}

float3 ShadeDomain(
    RestirDiTemporalReservoir r, float3 receiver, float3 geomN, float3 n, float3 v,
    float3 albedo, float metallic, float roughness)
{
    float3 f, wi; float dist;
    if (!EvaluateSample(r.sample, receiver, n, v, albedo, metallic, roughness, f, wi, dist))
        return 0.0.xxx;
    return f * r.W * Visibility(receiver, geomN, wi, dist);
}

bool GiReservoirValid(RestirGiReservoir r)
{
    return r.M > 0u && r.weightSum > 0.0 && isfinite(r.weightSum)
        && (r.flags & kRestirSampleNoReuse) == 0u
        && all(isfinite(r.position)) && all(isfinite(r.radiance));
}

// `receiver` is the ray-offset primary point used for the reconnection visibility ray; visibility
// is folded into the target so an occluded reused secondary cannot persist and inflate visible
// winners (the recursive brightening bias — see EvaluateShadowedTarget). A native fresh sample is
// visible by construction (visibility == 1), preserving M=1 parity.
float GiTarget(
    RestirGiReservoir r,
    float3 primaryPosition,
    float3 receiver,
    float3 primaryNormal,
    float3 primaryView,
    float3 albedo,
    float metallic,
    float roughness)
{
    if (!GiReservoirValid(r)) return 0.0;
    const float3 toSecondary = r.position - primaryPosition;
    const float distance = length(toSecondary);
    if (distance <= 1e-4) return 0.0;
    const float3 wi = toSecondary / distance;
    const float3 bsdfCos = EvaluateBsdfCos(
        primaryNormal, primaryView, wi, albedo, metallic, roughness);
    const float baseTarget = RestirDiTargetLuminance(bsdfCos * max(r.radiance, 0.0.xxx));
    if (baseTarget <= 0.0) return 0.0;
    const float3 toSecFromReceiver = r.position - receiver;
    const float visDist = length(toSecFromReceiver);
    if (visDist <= 0.002) return 0.0;
    return baseTarget * Visibility(
        receiver, 0.0.xxx, toSecFromReceiver / visDist, max(visDist - 0.003, 0.0011));
}

float GiTemporalJacobian(
    RestirGiReservoir r, float3 previousPrimary, float3 currentPrimary)
{
    const float3 secondaryNormal = RestirUnpackOctNormal(r.normalOct);
    const float3 toPrevious = previousPrimary - r.position;
    const float3 toCurrent = currentPrimary - r.position;
    const float previousDistance = length(toPrevious);
    const float currentDistance = length(toCurrent);
    if (previousDistance <= 1e-4 || currentDistance <= 1e-4) return 0.0;
    const float previousCos = dot(secondaryNormal, toPrevious / previousDistance);
    const float currentCos = dot(secondaryNormal, toCurrent / currentDistance);
    if (previousCos <= 1e-4 || currentCos <= 1e-4) return 0.0;
    const float jacobian = (currentCos / previousCos)
        * ((previousDistance * previousDistance) / (currentDistance * currentDistance));
    if (!isfinite(jacobian)
        || jacobian < kRestirJacobianMin || jacobian > kRestirJacobianMax)
    {
        return 0.0;
    }
    return jacobian;
}

void GiCopySelectedSample(inout RestirGiReservoir destination, RestirGiReservoir source)
{
    destination.position = source.position;
    destination.normalOct = source.normalOct;
    destination.radiance = source.radiance;
    destination.age = source.age;
    destination.flags = source.flags;
    destination.seed = source.seed;
    destination.instanceId = source.instanceId;
    destination.primitiveIndex = source.primitiveIndex;
}

bool GiCombine(
    inout RestirGiReservoir destination,
    RestirGiReservoir source,
    float targetAtCurrent,
    float random)
{
    if (!GiReservoirValid(source)) return false;
    const float risWeight = targetAtCurrent * source.weightSum * float(source.M);
    destination.M += source.M;
    destination.weightSum += max(risWeight, 0.0);
    const bool selected = risWeight > 0.0
        && random * destination.weightSum <= risWeight;
    if (selected) GiCopySelectedSample(destination, source);
    return selected;
}

RestirGiReservoir GiTemporalResample(
    RestirGiReservoir fresh,
    RestirGiReservoir previous,
    bool historyAccepted,
    uint2 pixel,
    float3 currentPrimary,
    float3 currentReceiver,
    float3 currentNormal,
    float3 currentView,
    float3 currentAlbedo,
    float currentMetallic,
    float currentRoughness,
    float3 previousPrimary,
    float3 previousReceiver,
    float3 previousNormal,
    float3 previousView,
    float3 previousAlbedo,
    float previousMetallic,
    float previousRoughness,
    out bool usedHistory,
    out bool jacobianRejected)
{
    RestirGiReservoir output = (RestirGiReservoir)0;
    // Preserve the original current-frame input through every reuse pass. RTXDI's final-shading
    // MIS evaluates this sample independently from the selected resampled output.
    output.initialPosition = fresh.initialPosition;
    output.initialNormalOct = fresh.initialNormalOct;
    output.initialRadiance = fresh.initialRadiance;
    output.initialWeightSum = fresh.initialWeightSum;
    usedHistory = false;
    jacobianRejected = false;

    const float freshTarget = GiTarget(
        fresh, currentPrimary, currentReceiver, currentNormal, currentView,
        currentAlbedo, currentMetallic, currentRoughness);
    float freshM = float(fresh.M);
    float previousM = 0.0;
    float selectedTarget = freshTarget;
    GiCombine(output, fresh, freshTarget, 0.5);

    bool previousAccepted = historyAccepted && GiReservoirValid(previous)
        && previous.age < kRestirAgeCap;
    if (previousAccepted)
    {
        const float jacobian = GiTemporalJacobian(previous, previousPrimary, currentPrimary);
        if (jacobian <= 0.0)
        {
            previousAccepted = false;
            jacobianRejected = true;
        }
        else
        {
            previous.weightSum *= jacobian;
            previous.M = min(previous.M, kRestirMCap);
            previousM = float(previous.M);
            previous.age = min(previous.age + 1u, kRestirAgeCap);
        }
    }

    bool selectedPrevious = false;
    float previousTargetAtCurrent = 0.0;
    if (previousAccepted)
    {
        previousTargetAtCurrent = GiTarget(
            previous, currentPrimary, currentReceiver, currentNormal, currentView,
            currentAlbedo, currentMetallic, currentRoughness);
        selectedPrevious = GiCombine(
            output, previous, previousTargetAtCurrent, Hash(pixel, 700u));
        if (selectedPrevious) selectedTarget = previousTargetAtCurrent;
        usedHistory = previousTargetAtCurrent > 0.0;
    }

    // Same-domain temporal combine: simple RIS UCW W = wSum / (M * selectedTarget).
    //
    // TEMPORAL reuse reprojects the SAME world surface point across frames, so the current and
    // previous source domains are identical and the selected sample's target is the same in both
    // (temporalP == selectedTarget). The RTXDI BASIC multi-source correction (piSum with a distinct
    // previous-domain temporalP) is only needed for genuinely different domains — i.e. SPATIAL reuse
    // (P7). Evaluating a distinct temporalP here from the previous frame's stored (jittered / noisy /
    // possibly grazing) surface made temporalP << selectedTarget for a bright held secondary at
    // lighting boundaries, crushing the valid weight and producing a hard DARK line (measured: mode
    // 26 dark step, bright held radiance in mode 27, killed by disabling history). With
    // temporalP == selectedTarget the BASIC formula collapses to exactly this simple UCW, which the
    // CPU chain test proves unbiased (tests/restir_temporal_chain_test.cpp). Restore a distinct
    // previous-domain evaluation only when GI spatial reuse (P7) introduces real cross-domain sources.
    (void)freshM; (void)previousM; (void)selectedPrevious;
    const float simpleDenom = float(max(output.M, 1u)) * selectedTarget;
    output.weightSum = simpleDenom > 0.0 && isfinite(simpleDenom)
        ? output.weightSum / simpleDenom
        : 0.0;
    // A shifted sample with no common target support must not erase the valid P5 path that was
    // removed from g_BaseRadiance. Restore it before final shading, just like visibility failure.
    if (!GiReservoirValid(output))
    {
        usedHistory = false;
        return fresh;
    }
    if (selectedPrevious) output.flags |= kRestirGiSampleTemporalReuse;
    else output.flags &= ~kRestirGiSampleTemporalReuse;
    return output;
}

float GiVisibility(RestirGiReservoir r, float3 receiver, float3 geomNormal)
{
    if (!GiReservoirValid(r)) return 0.0;
    const float3 toSecondary = r.position - receiver;
    const float distance = length(toSecondary);
    if (distance <= 0.002) return 0.0;
    return Visibility(
        receiver, geomNormal, toSecondary / distance, max(distance - 0.003, 0.0011));
}

float3 ShadeGi(
    RestirGiReservoir r,
    float3 primaryPosition,
    float3 receiver,
    float3 geomNormal,
    float3 shadingNormal,
    float3 viewDirection,
    float3 albedo,
    float metallic,
    float roughness)
{
    if (!GiReservoirValid(r)) return 0.0.xxx;
    const float3 toSecondary = r.position - receiver;
    const float distance = length(toSecondary);
    if (distance <= 0.002) return 0.0.xxx;
    const float3 wi = normalize(r.position - primaryPosition);
    const float3 bsdfCos = EvaluateBsdfCos(
        shadingNormal, viewDirection, wi,
        albedo, metallic, roughness);
    // Reconnection visibility (must match the visibility folded into GiTarget so the shaded value
    // and the RIS weight agree). A native fresh sample is visible, so this is 1 for M=1.
    const float vis = Visibility(
        receiver, geomNormal, toSecondary / distance, max(distance - 0.003, 0.0011));
    return bsdfCos * max(r.radiance, 0.0.xxx) * r.weightSum * vis;
}

// NVIDIA RTXDI final-shading MIS. ReSTIR GI intentionally roughens the comparison BRDF to avoid
// sparse, stable specular samples; the original P5 input receives the complementary weight.
// Applying the same scalar to the combined diffuse+specular signal is equivalent to the sample's
// split-BRDF implementation for this renderer's combined output.
float GiInitialMisWeight(
    float3 samplePosition,
    float3 primaryPosition,
    float3 shadingNormal,
    float3 viewDirection,
    float3 albedo,
    float metallic,
    float roughness)
{
    const float3 toSecondary = samplePosition - primaryPosition;
    const float distance = length(toSecondary);
    if (distance <= 0.002) return 0.0;
    const float3 wi = toSecondary / distance;
    const float3 trueBsdf = clamp(
        EvaluateBsdfCos(shadingNormal, viewDirection, wi, albedo, metallic, roughness),
        0.0.xxx, 1.0e4.xxx);
    const float3 roughBsdf = clamp(
        EvaluateBsdfCos(shadingNormal, viewDirection, wi, albedo, metallic, max(roughness, 0.3)),
        1.0e-4.xxx, 1.0e4.xxx);
    const float trueLum = RestirDiTargetLuminance(trueBsdf);
    const float weight = saturate(
        trueLum / max(RestirDiTargetLuminance(trueBsdf + roughBsdf), 1.0e-8));
    return weight * weight * weight;
}

float3 ShadeGiWithInputMis(
    RestirGiReservoir r,
    float3 primaryPosition,
    float3 receiver,
    float3 geomNormal,
    float3 shadingNormal,
    float3 viewDirection,
    float3 albedo,
    float metallic,
    float roughness)
{
    const float initialInputWeight = GiInitialMisWeight(
        r.initialPosition, primaryPosition, shadingNormal, viewDirection,
        albedo, metallic, roughness);

    RestirGiReservoir initial = (RestirGiReservoir)0;
    initial.position = r.initialPosition;
    initial.normalOct = r.initialNormalOct;
    initial.radiance = r.initialRadiance;
    initial.weightSum = r.initialWeightSum;
    initial.M = r.initialWeightSum > 0.0 ? 1u : 0u;
    const float3 initialContribution = ShadeGi(
        initial, primaryPosition, receiver, geomNormal, shadingNormal, viewDirection,
        albedo, metallic, roughness);
    // The pre-spatial boiling filter empties extreme reused reservoirs, matching RTXDI. Our P5
    // input is carried inside the same record instead of a separate path-tracer input texture, so
    // an empty output must fall back to that fresh input at full weight.
    if (!GiReservoirValid(r)) return initialContribution;

    const float finalInputWeight = GiInitialMisWeight(
        r.position, primaryPosition, shadingNormal, viewDirection,
        albedo, metallic, roughness);
    const float3 finalRadiance = ShadeGi(
        r, primaryPosition, receiver, geomNormal, shadingNormal, viewDirection,
        albedo, metallic, roughness) * (1.0 - finalInputWeight);
    const float3 initialRadiance = initialContribution * initialInputWeight;
    return finalRadiance + initialRadiance;
}

bool SameLightSample(RestirDiLightSample a, RestirDiLightSample b)
{
    return a.sampleType == b.sampleType && a.index0 == b.index0 && a.index1 == b.index1
        && all(abs(a.uv - b.uv) < 1e-6);
}

bool SameGiSample(RestirGiReservoir a, RestirGiReservoir b)
{
    return a.instanceId == b.instanceId && a.primitiveIndex == b.primitiveIndex
        && all(abs(a.position - b.position) < 1e-4);
}

[shader("raygeneration")]
void RestirTemporalRayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    if (any(pixel >= g_OutputSize)) return;
    uint index = pixel.y * g_OutputSize.x + pixel.x;
    RestirDiReservoirSet fresh = g_ReservoirCurrent[index];
    RestirDiReservoirSet outputSet = fresh;
    RestirGiReservoir freshGi = g_GiReservoirCurrent[index];
    RestirGiReservoir outputGi = freshGi;
    float4 currPos = g_CurrSurfacePositionDepth[pixel];
    uint4 currMat = g_CurrSurfaceMaterial[pixel];
    bool eligible = ((currMat.z >> 24u) & 7u) == 1u;
    int2 prevPixel = int2(-1, -1);
    if (g_HistoryValid != 0u && eligible)
    {
        float2 currNdc = PixelToNdc(pixel);
        float2 prevNdc = currNdc - g_Motion[pixel].xy;
        float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
        int2 projected = int2(floor(prevUv * float2(g_OutputSize)));
        const int2 offsets[5] = { int2(0,0), int2(1,0), int2(-1,0), int2(0,1), int2(0,-1) };
        [unroll] for (uint i = 0u; i < 5u; ++i)
        {
            int2 q = projected + offsets[i];
            if (q.x >= 0 && q.y >= 0 && q.x < int(g_OutputSize.x) && q.y < int(g_OutputSize.y)
                && SurfaceCompatible(currPos, currMat, g_PrevSurfacePositionDepth[q], g_PrevSurfaceMaterial[q]))
            { prevPixel = q; break; }
        }
    }

    bool historyAccepted = prevPixel.x >= 0;
    bool anyPreviousCandidateAccepted = false;
    bool giHistoryAccepted = false;
    bool giJacobianRejected = false;
    bool giVisibilityRejected = false;
    // S1-P2 camera-domain AOV. These are calculated only for debug mode 48 and are never fed
    // back into reservoir selection, acceptance, weights, or shading.
    float receiverAgreementError = 1.0;
    float targetAgreementError = 1.0;
    if (historyAccepted)
    {
        RestirDiReservoirSet previous = g_ReservoirPrev[uint(prevPixel.y) * g_OutputSize.x + uint(prevPixel.x)];
        float3 hitPosition = currPos.xyz;
        float3 geomN = RestirUnpackOctNormal(currMat.x);
        float3 n = RestirUnpackOctNormal(currMat.y);
        float3 v = normalize(g_CameraPos - hitPosition);
        float3 receiver = hitPosition + geomN
            * max(length(hitPosition - g_CameraPos) * 0.001, 0.002);
        float roughness = f16tof32(currMat.w >> 16u);
        float4 am = g_CurrAlbedoMetallic[pixel];
        float4 previousPos = g_PrevSurfacePositionDepth[prevPixel];
        uint4 previousMat = g_PrevSurfaceMaterial[prevPixel];
        float3 previousGeomN = RestirUnpackOctNormal(previousMat.x);
        float3 previousN = RestirUnpackOctNormal(previousMat.y);
        float3 previousV = normalize(g_PrevCameraPos - previousPos.xyz);
        float3 previousReceiver = previousPos.xyz + previousGeomN
            * max(length(previousPos.xyz - g_PrevCameraPos) * 0.001, 0.002);
        float previousRoughness = f16tof32(previousMat.w >> 16u);
        float4 previousAm = g_PrevAlbedoMetallic[prevPixel];
        if (g_DebugMode == 48u)
        {
            receiverAgreementError = length(receiver - previousReceiver)
                / max(max(length(receiver - g_CameraPos), length(previousReceiver - g_PrevCameraPos)), 1e-3);
            // DI stores independent emissive and environment reservoirs.  The AOV must cover
            // every fresh domain that has a valid sample; an emissive-only comparison would show
            // a false failure in an environment-lit static scene.
            bool anyComparableTarget = false;
            float maxTargetAgreementError = 0.0;
            float3 currentContribution; float3 currentWi; float currentDistance;
            float3 previousContribution; float3 previousWi; float previousDistance;
            [unroll] for (uint domain = 0u; domain < 2u; ++domain)
            {
                RestirDiTemporalReservoir freshDomain = fresh.emissive;
                if (domain == 1u) freshDomain = fresh.environment;
                const bool currentTargetValid = EvaluateSample(
                    freshDomain.sample, receiver, n, v, am.rgb, am.a, roughness,
                    currentContribution, currentWi, currentDistance);
                const bool previousTargetValid = EvaluateSample(
                    freshDomain.sample, previousReceiver, previousN, previousV,
                    previousAm.rgb, previousAm.a, previousRoughness,
                    previousContribution, previousWi, previousDistance);
                if (currentTargetValid && previousTargetValid)
                {
                    const float currentTarget = RestirDiTargetLuminance(currentContribution);
                    const float previousTarget = RestirDiTargetLuminance(previousContribution);
                    maxTargetAgreementError = max(
                        maxTargetAgreementError,
                        abs(currentTarget - previousTarget)
                            / max(max(currentTarget, previousTarget), 1e-4));
                    anyComparableTarget = true;
                }
            }
            // No fresh light can be compared only when both domains are invalid.  That is not a
            // current/previous disagreement; valid domains above carry the numeric proof.
            targetAgreementError = anyComparableTarget ? maxTargetAgreementError : 0.0;
        }
        if (g_EnableDiTemporal != 0u)
        {
            bool usedE, usedV;
            outputSet.emissive = ResampleDomain(
                fresh.emissive, previous.emissive, true, pixel, 10u,
                receiver, n, v, am.rgb, am.a, roughness,
                previousReceiver, previousGeomN, previousN, previousV,
                previousAm.rgb, previousAm.a, previousRoughness, usedE);
            outputSet.environment = ResampleDomain(
                fresh.environment, previous.environment, true, pixel, 20u,
                receiver, n, v, am.rgb, am.a, roughness,
                previousReceiver, previousGeomN, previousN, previousV,
                previousAm.rgb, previousAm.a, previousRoughness, usedV);
            anyPreviousCandidateAccepted = usedE || usedV;
        }
        if (g_EnableGiTemporal != 0u && GiReservoirValid(freshGi))
        {
            const RestirGiReservoir previousGi =
                g_GiReservoirPrev[uint(prevPixel.y) * g_OutputSize.x + uint(prevPixel.x)];
            const bool giPrimaryHistoryAccepted = GiPrimaryHistoryCompatible(
                currPos, previousPos, g_Motion[pixel].z);
            outputGi = GiTemporalResample(
                freshGi, previousGi, giPrimaryHistoryAccepted, pixel,
                hitPosition, receiver, n, v, am.rgb, am.a, roughness,
                previousPos.xyz, previousReceiver, previousN, previousV,
                previousAm.rgb, previousAm.a, previousRoughness,
                giHistoryAccepted, giJacobianRejected);
        }
    }
    // The post-selection visibility fallback is gone: reconnection visibility now lives inside
    // GiTarget (an occluded reused sample gets target 0 and can never win) and inside ShadeGi, so a
    // selected winner is always visible by construction. Replacing it with fresh here would instead
    // reintroduce bias.
    g_ReservoirCurrent[index] = outputSet;
    g_GiReservoirCurrent[index] = outputGi;

    if (g_ShadeOutput != 0u)
    {
        const float3 hitPosition = currPos.xyz;
        const float3 geomN = RestirUnpackOctNormal(currMat.x);
        const float3 n = RestirUnpackOctNormal(currMat.y);
        const float3 v = normalize(g_CameraPos - hitPosition);
        const float3 receiver = hitPosition + geomN
            * max(length(hitPosition - g_CameraPos) * 0.001, 0.002);
        const float roughness = f16tof32(currMat.w >> 16u);
        const float4 am = g_CurrAlbedoMetallic[pixel];
        float3 radiance = g_BaseRadiance[pixel].rgb
            + ShadeDomain(outputSet.emissive, receiver, geomN, n, v, am.rgb, am.a, roughness)
            + ShadeDomain(outputSet.environment, receiver, geomN, n, v, am.rgb, am.a, roughness)
            + ShadeGiWithInputMis(outputGi, hitPosition, receiver, geomN, n, v,
                am.rgb, am.a, roughness);
        const float lum = RestirDiTargetLuminance(radiance);
        radiance *= min(1.0, 64.0 / max(lum, 1e-6));
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(radiance, old.a);
    }

    if (g_DebugMode >= 14u && g_DebugMode <= 17u)
    {
        float3 debug = 0.0.xxx;
        if (g_DebugMode == 14u)
        {
            float m = float(max(outputSet.emissive.M, outputSet.environment.M))
                / float(kRestirDiTemporalMCap);
            debug = saturate(m).xxx;
        }
        else if (g_DebugMode == 15u)
        {
            float age = float(max(outputSet.emissive.age, outputSet.environment.age))
                / float(kRestirDiTemporalAgeCap);
            debug = saturate(age).xxx;
        }
        else if (g_DebugMode == 16u)
        {
            bool historySelected = historyAccepted
                && (!SameLightSample(outputSet.emissive.sample, fresh.emissive.sample)
                    || !SameLightSample(outputSet.environment.sample, fresh.environment.sample));
            debug = historySelected ? float3(0.1, 0.25, 1.0) : float3(0.1, 1.0, 0.2);
        }
        else
        {
            debug = !eligible ? float3(1.0, 0.0, 1.0)
                : (historyAccepted && anyPreviousCandidateAccepted
                    ? float3(0.1, 1.0, 0.2)
                    : float3(1.0, 0.1, 0.05));
        }
        float4 old = g_Output[pixel];
        g_Output[pixel] = float4(debug, old.a);
    }
    else if (g_DebugMode >= 20u && g_DebugMode <= 27u)
    {
        float3 debug = 0.0.xxx;
        if (g_DebugMode == 20u)
        {
            debug = saturate(float(outputGi.M) / float(kRestirMCap)).xxx;
        }
        else if (g_DebugMode == 21u)
        {
            debug = saturate(float(outputGi.age) / float(kRestirAgeCap)).xxx;
        }
        else if (g_DebugMode == 22u)
        {
            const bool selectedHistory = GiReservoirValid(freshGi)
                && !SameGiSample(outputGi, freshGi);
            debug = selectedHistory ? float3(0.1, 0.25, 1.0) : float3(0.1, 1.0, 0.2);
        }
        else if (g_DebugMode == 23u)
        {
            debug = !GiReservoirValid(freshGi) ? float3(1.0, 0.0, 1.0)
                : (giVisibilityRejected ? float3(0.65, 0.15, 0.85)
                    : (giJacobianRejected ? float3(1.0, 0.75, 0.05)
                    : (historyAccepted && giHistoryAccepted
                        ? float3(0.1, 1.0, 0.2)
                        : float3(1.0, 0.1, 0.05))));
        }
        else if (g_DebugMode == 24u)
        {
            // Reinhard-normalized UCW. A hard spatial step here = reused vs fresh weight bias;
            // per-frame flicker = weight variance.
            debug = saturate(outputGi.weightSum / (outputGi.weightSum + 4.0)).xxx;
        }
        else if (g_DebugMode == 25u)
        {
            // Isolated GI contribution (base excluded), tonemapped luminance.
            const float3 hitPosition = currPos.xyz;
            const float3 geomN = RestirUnpackOctNormal(currMat.x);
            const float3 n = RestirUnpackOctNormal(currMat.y);
            const float3 v = normalize(g_CameraPos - hitPosition);
            const float3 receiver = hitPosition + geomN
                * max(length(hitPosition - g_CameraPos) * 0.001, 0.002);
            const float roughness = f16tof32(currMat.w >> 16u);
            const float4 am = g_CurrAlbedoMetallic[pixel];
            const float3 gi = ShadeGiWithInputMis(
                outputGi, hitPosition, receiver, geomN, n, v, am.rgb, am.a, roughness);
            const float l = RestirDiTargetLuminance(gi);
            debug = (l / (l + 1.0)).xxx;
        }
        else if (g_DebugMode == 26u)
        {
            // Reuse-minus-fresh GI bias map. In REFERENCE accumulation this averages to the mean
            // bias: 0.5 = none, >0.5 (brighter) = reuse over-brightens, <0.5 = reuse darkens. The
            // reuse estimate is ShadeGiWithInputMis(outputGi); the fresh reference is the same shade
            // on the untouched P5 reservoir. Their difference IS the P6 error, localized per pixel.
            const float3 hitPosition = currPos.xyz;
            const float3 geomN = RestirUnpackOctNormal(currMat.x);
            const float3 n = RestirUnpackOctNormal(currMat.y);
            const float3 v = normalize(g_CameraPos - hitPosition);
            const float3 receiver = hitPosition + geomN
                * max(length(hitPosition - g_CameraPos) * 0.001, 0.002);
            const float roughness = f16tof32(currMat.w >> 16u);
            const float4 am = g_CurrAlbedoMetallic[pixel];
            const float lReuse = RestirDiTargetLuminance(ShadeGiWithInputMis(
                outputGi, hitPosition, receiver, geomN, n, v, am.rgb, am.a, roughness));
            const float lFresh = RestirDiTargetLuminance(ShadeGiWithInputMis(
                freshGi, hitPosition, receiver, geomN, n, v, am.rgb, am.a, roughness));
            const float d = lReuse - lFresh;
            debug = saturate(0.5 + 0.5 * (d / (abs(d) + 0.05))).xxx;
        }
        else
        {
            // Reused reservoir's stored secondary radiance (tonemapped luminance). Reveals whether
            // the held secondary's radiance is systematically off across the line vs the weight.
            const float l = RestirDiTargetLuminance(max(outputGi.radiance, 0.0.xxx));
            debug = (l / (l + 1.0)).xxx;
        }
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(debug, old.a);
    }
    else if (g_DebugMode == 48u)
    {
        // R/G are normalized errors, B marks a compatible previous receiver. Static agreement
        // must be (0,0,1); this has no authority outside the diagnostic view.
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(
            saturate(receiverAgreementError),
            saturate(targetAgreementError),
            historyAccepted ? 1.0 : 0.0,
            old.a);
    }
}

[shader("raygeneration")]
void RestirGiBoilingFilterRayGen()
{
    static const uint kTileSize = 16u;
    const uint2 tileBase = DispatchRaysIndex().xy * kTileSize;

    float weightSum = 0.0;
    uint weightCount = 0u;
    [loop]
    for (uint y = 0u; y < kTileSize; ++y)
    {
        [loop]
        for (uint x = 0u; x < kTileSize; ++x)
        {
            const uint2 pixel = tileBase + uint2(x, y);
            if (any(pixel >= g_OutputSize)) continue;
            const uint index = pixel.y * g_OutputSize.x + pixel.x;
            const RestirGiReservoir reservoir = g_GiReservoirPrev[index];
            if (!GiReservoirValid(reservoir)) continue;
            const float effectiveWeight =
                RestirDiTargetLuminance(max(reservoir.radiance, 0.0.xxx))
                * reservoir.weightSum;
            if (effectiveWeight > 0.0 && isfinite(effectiveWeight))
            {
                weightSum += effectiveWeight;
                weightCount++;
            }
        }
    }

    const float averageNonzeroWeight = weightCount > 0u
        ? weightSum / float(weightCount) : 0.0;
    const float boilingMultiplier = 10.0
        / clamp(g_SpatialFilterStrength, 1.0e-6, 1.0) - 9.0;

    [loop]
    for (uint writeY = 0u; writeY < kTileSize; ++writeY)
    {
        [loop]
        for (uint writeX = 0u; writeX < kTileSize; ++writeX)
        {
            const uint2 pixel = tileBase + uint2(writeX, writeY);
            if (any(pixel >= g_OutputSize)) continue;
            const uint index = pixel.y * g_OutputSize.x + pixel.x;
            g_ReservoirCurrent[index] = g_ReservoirPrev[index];

            RestirGiReservoir reservoir = g_GiReservoirPrev[index];
            reservoir.flags &= ~kRestirGiSampleBoilingFiltered;
            const float effectiveWeight = GiReservoirValid(reservoir)
                ? RestirDiTargetLuminance(max(reservoir.radiance, 0.0.xxx))
                    * reservoir.weightSum
                : 0.0;
            const float ratio = averageNonzeroWeight > 0.0
                ? effectiveWeight / averageNonzeroWeight : 0.0;
            reservoir.padding0 = asuint(ratio);
            if (g_SpatialFilterStrength > 0.0
                && effectiveWeight > averageNonzeroWeight * boilingMultiplier)
            {
                // Preserve the attached P5 fields so final shading can take the fresh fallback.
                reservoir.weightSum = 0.0;
                reservoir.M = 0u;
                reservoir.flags |= kRestirGiSampleBoilingFiltered;
            }
            g_GiReservoirCurrent[index] = reservoir;
        }
    }
}

[shader("raygeneration")]
void RestirSpatialRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (any(pixel >= g_OutputSize)) return;
    const uint index = pixel.y * g_OutputSize.x + pixel.x;
    const uint4 centerMat = g_CurrSurfaceMaterial[pixel];
    const float4 centerPos = g_CurrSurfacePositionDepth[pixel];
    const bool eligible = ((centerMat.z >> 24u) & 7u) == 1u;
    RestirDiReservoirSet center = g_ReservoirPrev[index];
    RestirDiReservoirSet outputSet = center;
    const RestirGiReservoir temporalGi = g_GiReservoirPrev[index];
    RestirGiReservoir outputGi = temporalGi;

    if (!eligible)
    {
        g_ReservoirCurrent[index] = outputSet;
        g_GiReservoirCurrent[index] = outputGi;
        if (g_DebugMode == 18u || g_DebugMode == 19u
            || (g_DebugMode >= 33u && g_DebugMode <= 45u))
        {
            const float4 old = g_Output[pixel];
            g_Output[pixel] = (g_DebugMode == 44u || g_DebugMode == 45u)
                ? 0.0.xxxx : float4(1.0, 0.0, 1.0, old.a);
        }
        return;
    }

    const float3 centerGeomN = RestirUnpackOctNormal(centerMat.x);
    const float3 centerN = RestirUnpackOctNormal(centerMat.y);
    const float3 centerV = normalize(g_CameraPos - centerPos.xyz);
    const float3 centerReceiver = centerPos.xyz + centerGeomN
        * max(length(centerPos.xyz - g_CameraPos) * 0.001, 0.002);
    const float centerRoughness = f16tof32(centerMat.w >> 16u);
    const float4 centerAm = g_CurrAlbedoMetallic[pixel];
    const bool diSmoothMetalFallback = centerAm.a > 0.5
        && centerRoughness < kRestirDiSpatialMetalRoughnessCutoff;

    const uint sourceLimit = min(g_SpatialSampleCount + 1u, 6u);
    int2 sourcePixels[6];
    sourcePixels[0] = int2(pixel);
    uint sourceCount = 1u;
    const float rotation = Hash(pixel, 100u + g_SpatialIteration * 17u) * 6.2831853;
    const float goldenAngle = 2.3999632;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex + 1u < sourceLimit; ++sampleIndex)
    {
        const float radius = g_SpatialRadius
            * sqrt((float(sampleIndex) + 0.5) / max(float(sourceLimit - 1u), 1.0));
        const float angle = rotation + float(sampleIndex) * goldenAngle;
        const int2 offset = int2(round(float2(cos(angle), sin(angle)) * radius));
        // Match RTXDI's ClampSamplePositionIntoView contract so a viewport-edge footprint does not
        // lose half of its proposals.
        const int2 q = clamp(
            int2(pixel) + offset,
            int2(0, 0),
            int2(g_OutputSize) - int2(1, 1));
        if (SpatialSurfaceCompatible(centerPos, centerMat,
                g_CurrSurfacePositionDepth[q], g_CurrSurfaceMaterial[q]))
        {
            // Keep clamped duplicates. RTXDI samples a fixed number of neighbor proposals and does
            // not deduplicate ClampSamplePositionIntoView results; dropping them lowers M in a
            // moving band near the viewport edge and makes the edge footprint visibly discontinuous.
            sourcePixels[sourceCount++] = q;
        }
    }

    bool anyNeighborAccepted = false;
    bool anyNeighborSelected = false;
    bool anyFilterHit = false;
    bool giAnyNeighborAccepted = false;
    bool giAnyNeighborSelected = false;
    bool giAnyFilterHit = (temporalGi.flags & kRestirGiSampleBoilingFiltered) != 0u;
    bool giAnyJacobianRejected = false;
    bool giAnyZeroTarget = false;
    bool giNormalizationFailed = false;
    uint giCompatibleNeighborCount = sourceCount - 1u;
    uint giUsefulSourceCount = 0u;
    uint giSelectedSourceDebug = 0u;
    int2 giSelectedPixel = int2(pixel);
    float giSelectedJacobian = 1.0;
    float giNormalizationFactor = 1.0;
    float giMaxFilterRatio = asfloat(temporalGi.padding0);
    float giBoilingMultiplier = 10.0
        / clamp(g_SpatialFilterStrength, 1e-6, 1.0) - 9.0;

    if (g_EnableGiTemporal != 0u && GiReservoirValid(temporalGi))
    {
        RestirGiReservoir combinedGi = (RestirGiReservoir)0;
        // The original current-frame input is not a resampling candidate. Preserve it verbatim for
        // final-shading input/output MIS, regardless of which spatial source wins.
        combinedGi.initialPosition = temporalGi.initialPosition;
        combinedGi.initialNormalOct = temporalGi.initialNormalOct;
        combinedGi.initialRadiance = temporalGi.initialRadiance;
        combinedGi.initialWeightSum = temporalGi.initialWeightSum;

        float giSourceM[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        uint giSelectedSource = 0u;
        float giSelectedTargetAtCenter = 0.0;

        [loop]
        for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
        {
            const int2 q = sourcePixels[sourceIndex];
            RestirGiReservoir sourceGi =
                g_GiReservoirPrev[uint(q.y) * g_OutputSize.x + uint(q.x)];
            giMaxFilterRatio = max(giMaxFilterRatio, asfloat(sourceGi.padding0));
            if ((sourceGi.flags & kRestirGiSampleBoilingFiltered) != 0u)
            {
                giAnyFilterHit = true;
            }
            if (!GiReservoirValid(sourceGi)) continue;

            const float4 sourcePos = g_CurrSurfacePositionDepth[q];
            const float jacobian = sourceIndex == 0u ? 1.0
                : GiTemporalJacobian(sourceGi, sourcePos.xyz, centerPos.xyz);
            if (jacobian <= 0.0)
            {
                if (sourceIndex > 0u) giAnyJacobianRejected = true;
                continue;
            }

            const float targetAtCenter = GiTarget(
                sourceGi, centerPos.xyz, centerReceiver, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness);
            if (targetAtCenter > 0.0)
            {
                giUsefulSourceCount++;
            }
            else if (sourceIndex > 0u)
            {
                giAnyZeroTarget = true;
            }
            const uint mBefore = combinedGi.M;
            const bool selected = GiCombine(
                combinedGi, sourceGi, targetAtCenter * jacobian,
                Hash(pixel, 500u + sourceIndex * 13u + g_SpatialIteration * 101u));
            giSourceM[sourceIndex] = float(combinedGi.M - mBefore);
            if (sourceIndex > 0u && giSourceM[sourceIndex] > 0.0)
                giAnyNeighborAccepted = true;
            if (selected)
            {
                giSelectedSource = sourceIndex;
                giSelectedSourceDebug = sourceIndex;
                giSelectedPixel = q;
                giSelectedJacobian = jacobian;
                giSelectedTargetAtCenter = targetAtCenter;
                if (sourceIndex > 0u) giAnyNeighborSelected = true;
            }
        }

        // RTXDI BASIC multi-source correction. Jacobians affect candidate streaming above; the
        // selected sample's target is then reevaluated in every accepted source receiver domain.
        float giPiSum = 0.0;
        float giSelectedSourceTarget = 0.0;
        [loop]
        for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
        {
            if (giSourceM[sourceIndex] <= 0.0) continue;
            const int2 q = sourcePixels[sourceIndex];
            const float4 sourcePos = g_CurrSurfacePositionDepth[q];
            const uint4 sourceMat = g_CurrSurfaceMaterial[q];
            const float3 sourceGeomN = RestirUnpackOctNormal(sourceMat.x);
            const float3 sourceN = RestirUnpackOctNormal(sourceMat.y);
            const float3 sourceV = normalize(g_CameraPos - sourcePos.xyz);
            const float3 sourceReceiver = sourcePos.xyz + sourceGeomN
                * max(length(sourcePos.xyz - g_CameraPos) * 0.001, 0.002);
            const float sourceRoughness = f16tof32(sourceMat.w >> 16u);
            const float4 sourceAm = g_CurrAlbedoMetallic[q];
            const float sourceTarget = GiTarget(
                combinedGi, sourcePos.xyz, sourceReceiver, sourceN, sourceV,
                sourceAm.rgb, sourceAm.a, sourceRoughness);
            giPiSum += sourceTarget * giSourceM[sourceIndex];
            if (sourceIndex == giSelectedSource) giSelectedSourceTarget = sourceTarget;
        }
        const float giDenominator = giSelectedTargetAtCenter * giPiSum;
        giNormalizationFactor = giDenominator > 0.0 && isfinite(giDenominator)
            && isfinite(giSelectedSourceTarget)
            ? giSelectedSourceTarget / giDenominator : 0.0;
        combinedGi.weightSum = giDenominator > 0.0 && isfinite(giDenominator)
            && isfinite(giSelectedSourceTarget)
            ? combinedGi.weightSum * giSelectedSourceTarget / giDenominator
            : 0.0;
        giNormalizationFailed = !GiReservoirValid(combinedGi);
        if (GiReservoirValid(combinedGi))
        {
            combinedGi.flags &= ~kRestirGiSampleSpatialReuse;
            if (giAnyNeighborSelected) combinedGi.flags |= kRestirGiSampleSpatialReuse;
            outputGi = combinedGi;
        }
    }

    if (g_EnableDiTemporal != 0u && !diSmoothMetalFallback)
    {
        [unroll]
        for (uint domain = 0u; domain < 2u; ++domain)
        {
            RestirDiTemporalReservoir combined = RestirDiTemporalInit();
            float sourceM[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            uint selectedSource = 0u;

            float nonzeroWeightSum = 0.0;
            uint nonzeroWeightCount = 0u;
            [loop]
            for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
            {
                const int2 q = sourcePixels[sourceIndex];
                const RestirDiReservoirSet sourceSet =
                    g_ReservoirPrev[uint(q.y) * g_OutputSize.x + uint(q.x)];
                RestirDiTemporalReservoir source = sourceSet.emissive;
                if (domain != 0u) source = sourceSet.environment;
                if (source.W > 0.0 && isfinite(source.W))
                {
                    nonzeroWeightSum += source.W;
                    nonzeroWeightCount++;
                }
            }
            const float averageNonzeroWeight = nonzeroWeightCount > 0u
                ? nonzeroWeightSum / float(nonzeroWeightCount) : 0.0;
            const float boilingMultiplier = 10.0
                / clamp(g_SpatialFilterStrength, 1e-6, 1.0) - 9.0;

        [loop]
        for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
        {
            const int2 q = sourcePixels[sourceIndex];
            const RestirDiReservoirSet sourceSet =
                g_ReservoirPrev[uint(q.y) * g_OutputSize.x + uint(q.x)];
            RestirDiTemporalReservoir source = sourceSet.emissive;
            if (domain != 0u) source = sourceSet.environment;
            if (g_SpatialFilterStrength > 0.0 && averageNonzeroWeight > 0.0
                && source.W > averageNonzeroWeight * boilingMultiplier)
            {
                anyFilterHit = true;
                continue;
            }
            float3 contribution, wi; float distance;
            float targetAtCenter = 0.0;
            if (EvaluateSample(source.sample, centerReceiver, centerN, centerV,
                    centerAm.rgb, centerAm.a, centerRoughness,
                    contribution, wi, distance))
            {
                targetAtCenter = RestirDiTargetLuminance(contribution);
            }
            const uint mBefore = combined.M;
            const bool selected = RestirDiTemporalCombine(
                combined, source, targetAtCenter,
                Hash(pixel, 200u + domain * 31u + sourceIndex * 7u + g_SpatialIteration * 101u));
            sourceM[sourceIndex] = float(combined.M - mBefore);
            if (sourceIndex > 0u && sourceM[sourceIndex] > 0.0) anyNeighborAccepted = true;
            if (selected)
            {
                selectedSource = sourceIndex;
                if (sourceIndex > 0u) anyNeighborSelected = true;
            }
        }

        if (combined.M > kRestirDiTemporalMCap)
        {
            const float capScale = float(kRestirDiTemporalMCap) / float(combined.M);
            combined.wSum *= capScale;
            [unroll] for (uint i = 0u; i < 6u; ++i) sourceM[i] *= capScale;
            combined.M = kRestirDiTemporalMCap;
        }

        // RTXDI basic multi-source correction. Evaluate the selected sample's target in every
        // source receiver domain, but defer binary visibility to the final center-domain shade.
        // Ray-traced source-domain correction creates moving rejection discontinuities at clipped
        // neighborhoods and is substantially more expensive; it is a separate optional policy.
        float piSum = 0.0;
        float selectedSourceTarget = 0.0;
        [loop]
        for (uint sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
        {
            if (sourceM[sourceIndex] <= 0.0) continue;
            const int2 q = sourcePixels[sourceIndex];
            const float4 sourcePos = g_CurrSurfacePositionDepth[q];
            const uint4 sourceMat = g_CurrSurfaceMaterial[q];
            const float3 sourceGeomN = RestirUnpackOctNormal(sourceMat.x);
            const float3 sourceN = RestirUnpackOctNormal(sourceMat.y);
            const float3 sourceV = normalize(g_CameraPos - sourcePos.xyz);
            const float3 sourceReceiver = sourcePos.xyz + sourceGeomN
                * max(length(sourcePos.xyz - g_CameraPos) * 0.001, 0.002);
            const float sourceRoughness = f16tof32(sourceMat.w >> 16u);
            const float4 sourceAm = g_CurrAlbedoMetallic[q];
            float3 contribution, wi; float distance;
            float sourceTarget = 0.0;
            if (EvaluateSample(combined.sample, sourceReceiver, sourceN, sourceV,
                    sourceAm.rgb, sourceAm.a, sourceRoughness,
                    contribution, wi, distance))
            {
                sourceTarget = RestirDiTargetLuminance(contribution);
            }
            piSum += sourceTarget * sourceM[sourceIndex];
            if (sourceIndex == selectedSource) selectedSourceTarget = sourceTarget;
        }
        const float denominator = combined.targetPdf * piSum;
        combined.W = denominator > 0.0 && isfinite(denominator)
            && isfinite(selectedSourceTarget)
            ? combined.wSum * selectedSourceTarget / denominator
            : 0.0;

            if (domain == 0u) outputSet.emissive = combined;
            else outputSet.environment = combined;
        }
    }

    g_ReservoirCurrent[index] = outputSet;
    g_GiReservoirCurrent[index] = outputGi;
    const bool needsPostSpatialGi = g_ShadeOutput != 0u
        || g_DebugMode == 36u || g_DebugMode == 37u
        || g_DebugMode == 44u || g_DebugMode == 45u;
    float3 postSpatialGi = 0.0.xxx;
    if (needsPostSpatialGi)
    {
        postSpatialGi = ShadeGiWithInputMis(
            outputGi, centerPos.xyz, centerReceiver, centerGeomN, centerN, centerV,
            centerAm.rgb, centerAm.a, centerRoughness);
    }
    if (g_ShadeOutput != 0u)
    {
        float3 radiance = g_BaseRadiance[pixel].rgb
            + ShadeDomain(outputSet.emissive, centerReceiver, centerGeomN, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness)
            + ShadeDomain(outputSet.environment, centerReceiver, centerGeomN, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness)
            + postSpatialGi;
        const float lum = RestirDiTargetLuminance(radiance);
        radiance *= min(1.0, 64.0 / max(lum, 1e-6));
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(radiance, old.a);
    }

    if (g_DebugMode == 18u || g_DebugMode == 19u)
    {
        const float3 debug = diSmoothMetalFallback ? float3(0.65, 0.15, 0.85)
            : (!eligible ? float3(1.0, 0.0, 1.0)
            : (g_DebugMode == 18u
                ? (anyNeighborSelected ? float3(0.1, 0.25, 1.0) : float3(0.1, 1.0, 0.2))
                : (anyFilterHit ? float3(1.0, 0.75, 0.05)
                    : (anyNeighborAccepted ? float3(0.1, 1.0, 0.2) : float3(1.0, 0.1, 0.05)))));
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(debug, old.a);
    }
    else if (g_DebugMode >= 33u && g_DebugMode <= 45u)
    {
        float3 debug = 0.0.xxx;
        if (g_DebugMode == 44u || g_DebugMode == 45u)
        {
            // The post-process statistics pass consumes raw GI and current linear depth. Keeping
            // this un-tonemapped makes the numeric static/motion metrics physically meaningful.
            g_Output[pixel] = float4(max(postSpatialGi, 0.0.xxx), centerPos.w);
            return;
        }
        else if (g_DebugMode == 43u)
        {
            if (!eligible)
            {
                debug = float3(1.0, 0.0, 1.0);
            }
            else if (giAnyFilterHit)
            {
                // A filter hit is deliberately unmistakable. The previous packed-RGB view
                // made the fixed threshold look green and the ratio look yellow, which was
                // easy to misread as a rejection map.
                debug = float3(1.0, 0.03, 0.01);
            }
            else
            {
                const float thresholdFraction = giBoilingMultiplier > 0.0
                    ? saturate(giMaxFilterRatio / giBoilingMultiplier)
                    : 0.0;
                const float3 lowScore = float3(0.02, 0.08, 0.18);
                const float3 midScore = float3(0.05, 0.65, 0.18);
                const float3 nearThreshold = float3(1.0, 0.85, 0.05);
                debug = thresholdFraction < 0.5
                    ? lerp(lowScore, midScore, thresholdFraction * 2.0)
                    : lerp(midScore, nearThreshold, (thresholdFraction - 0.5) * 2.0);
            }
        }
        else if (!GiReservoirValid(temporalGi)
            && (temporalGi.flags & kRestirGiSampleBoilingFiltered) == 0u)
        {
            debug = float3(1.0, 0.0, 1.0);
        }
        else if (g_DebugMode == 33u)
        {
            if (giSelectedSourceDebug == 0u)
            {
                debug = float3(0.1, 1.0, 0.2);
            }
            else
            {
                const float2 normalizedOffset = clamp(
                    float2(giSelectedPixel - int2(pixel)) / max(g_SpatialRadius, 1.0),
                    -1.0.xx,
                    1.0.xx);
                debug = float3(normalizedOffset * 0.5 + 0.5, float(giSelectedSourceDebug) / 5.0);
            }
        }
        else if (g_DebugMode == 34u)
        {
            debug = giAnyFilterHit ? float3(1.0, 0.75, 0.05)
                : (giAnyJacobianRejected ? float3(1.0, 0.35, 0.05)
                : (giNormalizationFailed ? float3(0.65, 0.15, 0.85)
                : (giAnyZeroTarget ? float3(0.05, 0.8, 1.0)
                : (giUsefulSourceCount > 1u ? float3(0.1, 1.0, 0.2)
                : float3(1.0, 0.1, 0.05)))));
        }
        else if (g_DebugMode == 35u)
        {
            debug = saturate(outputGi.weightSum / (outputGi.weightSum + 4.0)).xxx;
        }
        else if (g_DebugMode == 36u)
        {
            debug = max(postSpatialGi, 0.0.xxx) / (max(postSpatialGi, 0.0.xxx) + 1.0.xxx);
        }
        else if (g_DebugMode == 37u)
        {
            const float3 temporalContribution = ShadeGiWithInputMis(
                temporalGi, centerPos.xyz, centerReceiver, centerGeomN, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness);
            const float delta = RestirDiTargetLuminance(postSpatialGi)
                - RestirDiTargetLuminance(temporalContribution);
            debug = saturate(0.5 + 0.5 * (delta / (abs(delta) + 0.05))).xxx;
        }
        else if (g_DebugMode == 38u)
        {
            const float effectiveWeight =
                RestirDiTargetLuminance(max(outputGi.radiance, 0.0.xxx)) * outputGi.weightSum;
            debug = saturate(effectiveWeight / (effectiveWeight + 4.0)).xxx;
        }
        else if (g_DebugMode == 39u)
        {
            debug = SignedLogRatioColor(giSelectedJacobian);
        }
        else if (g_DebugMode == 40u)
        {
            debug = giNormalizationFactor > 0.0
                ? SignedLogRatioColor(giNormalizationFactor) : float3(0.65, 0.15, 0.85);
        }
        else if (g_DebugMode == 41u)
        {
            const float finalInputWeight = GiInitialMisWeight(
                outputGi.position, centerPos.xyz, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness);
            const float initialInputWeight = GiInitialMisWeight(
                outputGi.initialPosition, centerPos.xyz, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness);
            debug = float3(initialInputWeight, 1.0 - finalInputWeight, finalInputWeight);
        }
        else if (g_DebugMode == 42u)
        {
            debug = float3(
                float(giCompatibleNeighborCount) / 5.0,
                float(giUsefulSourceCount) / max(float(sourceCount), 1.0),
                float(giSelectedSourceDebug) / 5.0);
        }
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(debug, old.a);
    }
}

[shader("miss")]
void RestirMiss(inout Payload payload) { payload.hit = 0u; }

[shader("closesthit")]
void RestirClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    (void)attribs; payload.hit = 1u;
}
