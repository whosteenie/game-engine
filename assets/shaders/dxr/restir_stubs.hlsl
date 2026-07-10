// ReSTIR GI temporal reuse (R2) + spatial stub (R3) — devdoc/dxr/pt/restir-pt.md.
// Temporal: reproject → surface validate → M-cap merge + pairwise MIS → visibility → boil → shade.

#include "restir_pack.hlsli"

cbuffer RestirTemporalConstants : register(b0)
{
    uint2 g_OutputSize;
    uint g_HistoryValid; // prev surface + prev reservoir ping available
    uint g_FrameIndex;
    float4x4 g_InvViewProj; // jittered, matches PT depth
    float3 g_CameraPos;
    float g_MaxTraceDistance;
    uint g_ShadeOutput; // 0 = reservoirs only (isolate AOVs); 1 = rewrite g_Output
    uint3 g_ShadePad;
};

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_PrevDepth : register(t1);
Texture2D<float4> g_PrevNormalRoughness : register(t2);
Texture2D<float> g_CurrDepth : register(t3);
Texture2D<float4> g_CurrNormalRoughness : register(t4);
Texture2D<float4> g_Motion : register(t5);
Texture2D<float4> g_Direct : register(t6); // PT bounce-0 direct (R2 shade)

RWStructuredBuffer<RestirReservoir> g_ReservoirCurrent : register(u0);
RWStructuredBuffer<RestirReservoir> g_ReservoirPrev : register(u1);
RWStructuredBuffer<RestirInitialSample> g_InitialSample : register(u2);
RWTexture2D<float4> g_Output : register(u3);

struct Payload
{
    // TraceVisibility convention: pre-set non-zero; miss clears to 0 (visible).
    uint hit;
};

static const uint kPayloadFlagVisibility = 2u;

float TraceRestirVisibility(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    Payload probe;
    probe.hit = kPayloadFlagVisibility;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
}

float2 PixelToNdc(uint2 pixel, uint2 size)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(size);
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 NdcToUv(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float3 ReconstructWorldPos(uint2 pixel, float depth)
{
    const float2 ndc = PixelToNdc(pixel, g_OutputSize);
    const float4 worldH = mul(g_InvViewProj, float4(ndc, depth, 1.0));
    return worldH.xyz / max(worldH.w, 1e-6);
}

// Finite-difference geometric normal from PT depth — more stable than RR shading-normal guides
// for horizon tests and validation-ray origins (Codex R2 lead).
float3 ReconstructGeometricNormal(uint2 pixel, float depth)
{
    const int2 p = int2(pixel);
    const int2 px = int2(min(pixel.x + 1u, g_OutputSize.x - 1u), pixel.y);
    const int2 py = int2(pixel.x, min(pixel.y + 1u, g_OutputSize.y - 1u));
    const float3 pos = ReconstructWorldPos(pixel, depth);
    const float3 posX = ReconstructWorldPos(uint2(px), g_CurrDepth[px]);
    const float3 posY = ReconstructWorldPos(uint2(py), g_CurrDepth[py]);
    float3 geoN = cross(posY - pos, posX - pos);
    const float lenSq = dot(geoN, geoN);
    if (lenSq < 1e-12)
    {
        return float3(0.0, 1.0, 0.0);
    }
    geoN = normalize(geoN);
    // Face toward camera so grazing/backface depth flips don't invert the offset.
    if (dot(geoN, normalize(g_CameraPos - pos)) < 0.0)
    {
        geoN = -geoN;
    }
    return geoN;
}

bool RestirSurfaceSimilar(
    float currDepth,
    float prevDepth,
    float3 currN,
    float3 prevN,
    float currRough,
    float prevRough)
{
    if (currDepth >= 0.9999 || prevDepth >= 0.9999)
    {
        return false;
    }

    // Hyperbolic depth: relative gate ≈ plan's 2% linear for mid-field hits.
    const float depthDiff = abs(currDepth - prevDepth);
    if (depthDiff > 0.02 * max(max(currDepth, prevDepth), 1e-3))
    {
        return false;
    }

    // Slightly softer than 0.9 — RR guides can be normal-mapped / transmission-lerped.
    if (dot(currN, prevN) < 0.85)
    {
        return false;
    }

    if (abs(currRough - prevRough) > 0.1)
    {
        return false;
    }

    return true;
}

float RestirHash(uint2 pixel, uint frame, uint salt)
{
    uint n = pixel.x * 1664525u + pixel.y * 1013904223u + frame * 747796405u + salt * 2891336453u;
    n = (n ^ (n >> 16)) * 0x45d9f3bu;
    n = (n ^ (n >> 16)) * 0x45d9f3bu;
    n = n ^ (n >> 16);
    return float(n & 0x00ffffffu) / float(0x01000000u);
}

[shader("raygeneration")]
void RestirTemporalRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const uint pixelIndex = pixel.y * g_OutputSize.x + pixel.x;
    const RestirInitialSample initial = g_InitialSample[pixelIndex];
    const float3 yInitial = RestirUnpackLoTail(initial);
    const uint flags = RestirSampleFlags(initial);

    RestirReservoir outRes = RestirMakePassthroughReservoir(initial);

    const bool skipReuse = (flags & kRestirSampleNoReuse) != 0u;
    bool reused = false;

    if (g_HistoryValid != 0u && !skipReuse)
    {
        const float2 currNdc = PixelToNdc(pixel, g_OutputSize);
        const float2 motion = g_Motion[pixel].xy; // currNdc - prevNdc
        const float2 prevNdc = currNdc - motion;
        const float2 prevUv = NdcToUv(prevNdc);
        const int2 prevPixel = int2(floor(prevUv * float2(g_OutputSize)));

        if (prevPixel.x >= 0 && prevPixel.y >= 0
            && prevPixel.x < int(g_OutputSize.x) && prevPixel.y < int(g_OutputSize.y))
        {
            const float currDepth = g_CurrDepth[pixel];
            const float4 currNr = g_CurrNormalRoughness[pixel];
            const float prevDepth = g_PrevDepth[prevPixel];
            const float4 prevNr = g_PrevNormalRoughness[prevPixel];

            if (RestirSurfaceSimilar(
                    currDepth,
                    prevDepth,
                    normalize(currNr.xyz),
                    normalize(prevNr.xyz),
                    currNr.w,
                    prevNr.w))
            {
                const uint prevIndex = uint(prevPixel.y) * g_OutputSize.x + uint(prevPixel.x);
                RestirReservoir prevRes = g_ReservoirPrev[prevIndex];
                const uint prevFlags = RestirSampleFlags(prevRes.sample);

                if (prevRes.M > 0u && (prevFlags & kRestirSampleNoReuse) == 0u)
                {
                    // M-cap confidence clamp (restir-pt.md §3).
                    prevRes.M = min(prevRes.M, kRestirMCap);

                    // Confidence-weighted WRS (no pairwise MIS). Y is already the MC contribution
                    // with passthrough W=1; mis=M_i/ΣM *then* ×M in Merge double-counts and drives
                    // W→0.5 on early frames — validation flicker becomes salt-and-pepper.
                    RestirReservoir merged;
                    merged.sample = initial;
                    merged.wSum = 0.0;
                    merged.W = 0.0;
                    merged.M = 0u;
                    merged.pad = 0u;

                    RestirUpdate(
                        merged,
                        initial,
                        max(RestirLuminance(yInitial), 1e-6),
                        RestirHash(pixel, g_FrameIndex, 1u));

                    RestirMergeReservoir(
                        merged,
                        prevRes,
                        1.0,
                        RestirHash(pixel, g_FrameIndex, 2u));

                    RestirFinalizeW(merged);

                    // Validation ray: current primary → kept xs (unshadowed p̂; V enters here).
                    const float3 primaryPos = ReconstructWorldPos(pixel, currDepth);
                    const float3 primaryN = ReconstructGeometricNormal(pixel, currDepth);
                    const float3 toXs = merged.sample.xs - primaryPos;
                    const float dist = length(toXs);
                    bool visible = dist > 1e-4;
                    if (visible)
                    {
                        const float3 wi = toXs / dist;
                        if (dot(primaryN, wi) <= 0.0)
                        {
                            visible = false;
                        }
                        else
                        {
                            const float3 origin = primaryPos + primaryN * max(dist * 0.001, 0.002);
                            visible = TraceRestirVisibility(origin, wi, dist - 0.001) > 0.0;
                        }
                    }

                    if (visible)
                    {
                        // Boiling filter vs this pixel's fresh sample (cheap 1-tap stand-in for 3×3).
                        const float keptStrength =
                            merged.W * max(RestirLuminance(RestirUnpackLoTail(merged.sample)), 1e-6);
                        const float localStrength = max(RestirLuminance(yInitial), 1e-6);
                        if (keptStrength <= kRestirBoilingFactor * localStrength)
                        {
                            outRes = merged;
                            reused = true;
                        }
                    }
                }
            }
        }
    }

    if (!reused)
    {
        outRes = RestirMakePassthroughReservoir(initial);
    }

    g_ReservoirCurrent[pixelIndex] = outRes;

    // Only rewrite when temporal reuse actually replaced the sample. Passthrough must keep the
    // PT megakernel's ClampRadiance'd g_Output — reconstructing direct+fp16(Y) reintroduces
    // underside salt-and-pepper (same class of bug as the old rgb−Y path).
    if (g_ShadeOutput != 0u && reused)
    {
        const float4 prevOut = g_Output[pixel];
        const float3 direct = g_Direct[pixel].rgb;
        float w = outRes.W;
        w = (w == w && w > 0.0) ? w : 0.0;
        const float3 shaded =
            RestirClampRadiance(direct + RestirUnpackLoTail(outRes.sample) * w);
        g_Output[pixel] = float4(shaded, prevOut.a);
    }
    else
    {
        (void)yInitial;
        (void)g_Direct;
        (void)g_Output;
    }
}

[shader("raygeneration")]
void RestirSpatialRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    // R3 stub — spatial reuse not yet dispatched.
    (void)g_SceneTlas;
    (void)g_ReservoirCurrent;
    (void)g_ReservoirPrev;
    (void)g_InitialSample;
    (void)g_Direct;
    (void)g_Output;
}

[shader("miss")]
void RestirMiss(inout Payload payload)
{
    payload.hit = 0;
}

[shader("closesthit")]
void RestirClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit = 1;
    (void)attribs;
}
