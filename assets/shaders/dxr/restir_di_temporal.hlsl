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
    uint3 _PadDebug;
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
RWStructuredBuffer<RestirInitialSample> g_UnusedInitialSample : register(u2);
RWTexture2D<float4> g_Output : register(u3);

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
    if (abs(aPos.w - bPos.w) > 0.02 * max(max(aPos.w, bPos.w), 1e-3))
        return false;
    if (dot(RestirUnpackOctNormal(aMat.y), RestirUnpackOctNormal(bMat.y)) < 0.9)
        return false;
    if ((aMat.w & 0xffffu) != (bMat.w & 0xffffu))
        return false;
    return abs(f16tof32(aMat.w >> 16u) - f16tof32(bMat.w >> 16u)) <= 0.1;
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
        if (dot(EquirectUvToDirection(uv), normalize(g_SunDirection)) >= cosBoundary)
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
        wi = EquirectUvToDirection(sample.uv); distance = g_MaxTraceDistance;
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

RestirDiTemporalReservoir ResampleDomain(
    RestirDiTemporalReservoir fresh, RestirDiTemporalReservoir previous,
    bool historyAccepted, uint2 pixel, uint salt,
    float3 receiver, float3 n, float3 v, float3 albedo, float metallic, float roughness,
    float3 previousReceiver, float3 previousGeomN, float3 previousN, float3 previousV,
    float3 previousAlbedo, float previousMetallic, float previousRoughness,
    out bool usedHistory)
{
    RestirDiTemporalReservoir outR = RestirDiTemporalInit();
    float3 freshF, freshWi; float freshDist;
    float freshTarget = 0.0;
    if (EvaluateSample(fresh.sample, receiver, n, v, albedo, metallic, roughness,
            freshF, freshWi, freshDist))
    {
        freshTarget = RestirDiTargetLuminance(freshF);
    }
    const uint mBeforeFresh = outR.M;
    bool selectedPrevious = false;
    RestirDiTemporalCombine(outR, fresh, freshTarget, Hash(pixel, salt));
    float freshM = float(outR.M - mBeforeFresh);
    float previousM = 0.0;
    usedHistory = false;
    if (historyAccepted && previous.M > 0u && previous.age < kRestirDiTemporalAgeCap)
    {
        float3 f, wi; float dist;
        float previousTargetAtReceiver = 0.0;
        if (EvaluateSample(previous.sample, receiver, n, v, albedo, metallic, roughness, f, wi, dist))
        {
            previousTargetAtReceiver = RestirDiTargetLuminance(f);
        }
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

    // RTXDI BASIC temporal bias correction. Reevaluate the selected output sample at every
    // source receiver, then normalize by the source mixture that could have selected it.
    float selectedPreviousTarget = 0.0;
    float3 previousF, previousWi; float previousDistance;
    if (previousM > 0.0 && EvaluateSample(
            outR.sample, previousReceiver, previousN, previousV,
            previousAlbedo, previousMetallic, previousRoughness,
            previousF, previousWi, previousDistance))
    {
        // RTXDI BASIC correction uses target density only. Visibility is evaluated once for the
        // selected sample at the current receiver in ShadeDomain; source-domain visibility would
        // be the separate ray-traced correction mode and causes binary temporal weight changes.
        selectedPreviousTarget = RestirDiTargetLuminance(previousF);
    }
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

bool SameLightSample(RestirDiLightSample a, RestirDiLightSample b)
{
    return a.sampleType == b.sampleType && a.index0 == b.index0 && a.index1 == b.index1
        && all(abs(a.uv - b.uv) < 1e-6);
}

[shader("raygeneration")]
void RestirTemporalRayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    if (any(pixel >= g_OutputSize)) return;
    uint index = pixel.y * g_OutputSize.x + pixel.x;
    RestirDiReservoirSet fresh = g_ReservoirCurrent[index];
    RestirDiReservoirSet outputSet = fresh;
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
        if (g_ShadeOutput != 0u && (usedE || usedV))
        {
            float3 radiance = g_BaseRadiance[pixel].rgb
                + ShadeDomain(outputSet.emissive, receiver, geomN, n, v, am.rgb, am.a, roughness)
                + ShadeDomain(outputSet.environment, receiver, geomN, n, v, am.rgb, am.a, roughness);
            float lum = RestirDiTargetLuminance(radiance);
            radiance *= min(1.0, 64.0 / max(lum, 1e-6));
            float4 old = g_Output[pixel];
            g_Output[pixel] = float4(radiance, old.a);
        }
    }
    g_ReservoirCurrent[index] = outputSet;

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

    if (!eligible)
    {
        g_ReservoirCurrent[index] = outputSet;
        return;
    }

    const float3 centerGeomN = RestirUnpackOctNormal(centerMat.x);
    const float3 centerN = RestirUnpackOctNormal(centerMat.y);
    const float3 centerV = normalize(g_CameraPos - centerPos.xyz);
    const float3 centerReceiver = centerPos.xyz + centerGeomN
        * max(length(centerPos.xyz - g_CameraPos) * 0.001, 0.002);
    const float centerRoughness = f16tof32(centerMat.w >> 16u);
    const float4 centerAm = g_CurrAlbedoMetallic[pixel];
    if (centerAm.a > 0.5 && centerRoughness < kRestirDiSpatialMetalRoughnessCutoff)
    {
        // Production rough/specular fallback: keep the valid temporal estimate on smooth metals.
        g_ReservoirCurrent[index] = outputSet;
        if (g_DebugMode == 18u || g_DebugMode == 19u)
        {
            const float3 debug = g_DebugMode == 18u
                ? float3(0.1, 1.0, 0.2) : float3(0.65, 0.15, 0.85);
            const float4 old = g_Output[pixel];
            g_Output[pixel] = float4(debug, old.a);
        }
        return;
    }

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
        const float boilingMultiplier = 10.0 / clamp(g_SpatialFilterStrength, 1e-6, 1.0) - 9.0;

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

    g_ReservoirCurrent[index] = outputSet;
    if (g_ShadeOutput != 0u)
    {
        float3 radiance = g_BaseRadiance[pixel].rgb
            + ShadeDomain(outputSet.emissive, centerReceiver, centerGeomN, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness)
            + ShadeDomain(outputSet.environment, centerReceiver, centerGeomN, centerN, centerV,
                centerAm.rgb, centerAm.a, centerRoughness);
        const float lum = RestirDiTargetLuminance(radiance);
        radiance *= min(1.0, 64.0 / max(lum, 1e-6));
        const float4 old = g_Output[pixel];
        g_Output[pixel] = float4(radiance, old.a);
    }

    if (g_DebugMode == 18u || g_DebugMode == 19u)
    {
        const float3 debug = !eligible ? float3(1.0, 0.0, 1.0)
            : (g_DebugMode == 18u
                ? (anyNeighborSelected ? float3(0.1, 0.25, 1.0) : float3(0.1, 1.0, 0.2))
                : (anyFilterHit ? float3(1.0, 0.75, 0.05)
                    : (anyNeighborAccepted ? float3(0.1, 1.0, 0.2) : float3(1.0, 0.1, 0.05))));
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
