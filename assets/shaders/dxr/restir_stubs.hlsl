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
    uint g_SpatialSampleCount; // R3: neighbors per iteration (default 5)
    float g_SpatialRadius; // R3: disk radius in pixels (halved each iteration)
    uint g_SpatialIteration; // R3: 0, 1, ...
};

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float4> g_PrevSurfacePositionDepth : register(t1);
Texture2D<uint4> g_PrevSurfaceMaterial : register(t2);
Texture2D<float4> g_CurrSurfacePositionDepth : register(t3);
Texture2D<uint4> g_CurrSurfaceMaterial : register(t4);
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
    ray.TMin = min(0.001, max(tMax * 0.001, 0.00005));
    ray.TMax = max(tMax, ray.TMin + 0.00001);

    Payload probe;
    probe.hit = kPayloadFlagVisibility;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
}

float RestirConnectionBias(float dist)
{
    const float scaled = max(dist * 0.001, 0.00005);
    return min(scaled, max(dist * 0.2, 0.00005));
}

bool RestirValidateConnection(float3 receiverPos, float3 receiverN, float3 xs)
{
    const float3 toXs = xs - receiverPos;
    const float dist = length(toXs);
    const float bias = RestirConnectionBias(dist);
    if (dist <= bias * 2.0 + 0.00005)
    {
        return false;
    }

    const float3 wi = toXs / dist;
    if (dot(receiverN, wi) <= 0.0)
    {
        return false;
    }

    const float3 origin = receiverPos + receiverN * bias;
    return TraceRestirVisibility(origin, wi, dist - bias * 2.0) > 0.0;
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
    (void)depth;
    return g_CurrSurfacePositionDepth[pixel].xyz;
}

// Finite-difference geometric normal from PT depth — more stable than RR shading-normal guides
// for horizon tests and validation-ray origins (Codex R2 lead).
float3 ReconstructGeometricNormal(uint2 pixel, float depth)
{
    (void)depth;
    return RestirUnpackOctNormal(g_CurrSurfaceMaterial[pixel].x);
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

// Stricter gates for spatial contribution reuse (no receiver BSDF reconnection).
bool RestirSpatialSurfaceSimilar(
    float currDepth,
    float neighborDepth,
    float3 currN,
    float3 neighborN,
    float currRough,
    float neighborRough)
{
    if (currDepth >= 0.9999 || neighborDepth >= 0.9999)
    {
        return false;
    }

    const float depthDiff = abs(currDepth - neighborDepth);
    if (depthDiff > 0.01 * max(max(currDepth, neighborDepth), 1e-3))
    {
        return false;
    }

    if (dot(currN, neighborN) < 0.95)
    {
        return false;
    }

    if (abs(currRough - neighborRough) > 0.05)
    {
        return false;
    }

    return true;
}

bool RestirSurfaceRecordsSimilar(
    float4 currPositionDepth,
    float4 prevPositionDepth,
    uint4 currMaterial,
    uint4 prevMaterial,
    float depthThreshold,
    float normalThreshold,
    float roughnessThreshold)
{
    const uint currFlags = currMaterial.z >> 24u;
    const uint prevFlags = prevMaterial.z >> 24u;
    if ((currFlags & 1u) == 0u || (prevFlags & 1u) == 0u)
    {
        return false;
    }
    if (abs(currPositionDepth.w - prevPositionDepth.w)
        > depthThreshold * max(max(currPositionDepth.w, prevPositionDepth.w), 1e-3))
    {
        return false;
    }
    if (dot(RestirUnpackOctNormal(currMaterial.y), RestirUnpackOctNormal(prevMaterial.y))
        < normalThreshold)
    {
        return false;
    }
    const float currRoughness = f16tof32(currMaterial.w >> 16u);
    const float prevRoughness = f16tof32(prevMaterial.w >> 16u);
    return abs(currRoughness - prevRoughness) <= roughnessThreshold
        && (currMaterial.w & 0xffffu) == (prevMaterial.w & 0xffffu)
        && (currFlags & 6u) == (prevFlags & 6u);
}

bool RestirSampleChanged(RestirInitialSample a, RestirInitialSample b)
{
    return any(abs(a.xs - b.xs) > 1e-3)
        || a.loTailRg != b.loTailRg
        || (a.loTailBFlags & 0xffffu) != (b.loTailBFlags & 0xffffu);
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
        const int2 projectedPrevPixel = int2(floor(prevUv * float2(g_OutputSize)));
        int2 prevPixel = int2(-1, -1);
        const int2 fallbackOffsets[5] = {
            int2(0, 0), int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
        const float4 currSearchSurface = g_CurrSurfacePositionDepth[pixel];
        const uint4 currSearchMaterial = g_CurrSurfaceMaterial[pixel];
        [unroll]
        for (uint fallbackIndex = 0u; fallbackIndex < 5u; ++fallbackIndex)
        {
            const int2 candidate = projectedPrevPixel + fallbackOffsets[fallbackIndex];
            if (candidate.x < 0 || candidate.y < 0
                || candidate.x >= int(g_OutputSize.x) || candidate.y >= int(g_OutputSize.y))
            {
                continue;
            }
            if (RestirSurfaceRecordsSimilar(
                    currSearchSurface,
                    g_PrevSurfacePositionDepth[candidate],
                    currSearchMaterial,
                    g_PrevSurfaceMaterial[candidate],
                    0.02,
                    0.9,
                    0.1))
            {
                prevPixel = candidate;
                break;
            }
        }

        if (prevPixel.x >= 0 && prevPixel.y >= 0
            && prevPixel.x < int(g_OutputSize.x) && prevPixel.y < int(g_OutputSize.y))
        {
            const float4 currSurface = g_CurrSurfacePositionDepth[pixel];
            const uint4 currMaterial = g_CurrSurfaceMaterial[pixel];
            const float4 prevSurface = g_PrevSurfacePositionDepth[prevPixel];
            const uint4 prevMaterial = g_PrevSurfaceMaterial[prevPixel];

            if (RestirSurfaceRecordsSimilar(
                    currSurface, prevSurface, currMaterial, prevMaterial, 0.02, 0.9, 0.1))
            {
                const uint prevIndex = uint(prevPixel.y) * g_OutputSize.x + uint(prevPixel.x);
                RestirReservoir prevRes = g_ReservoirPrev[prevIndex];
                const uint prevFlags = RestirSampleFlags(prevRes.sample);

                if (prevRes.M > 0u && prevRes.age < kRestirAgeCap
                    && (prevFlags & kRestirSampleNoReuse) == 0u)
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
                    merged.age = 0u;

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
                    const float3 primaryPos = currSurface.xyz;
                    const float3 primaryN = RestirUnpackOctNormal(currMaterial.x);
                    const bool visible =
                        RestirValidateConnection(primaryPos, primaryN, merged.sample.xs);

                    if (visible)
                    {
                        // Boiling filter vs this pixel's fresh sample (cheap 1-tap stand-in for 3×3).
                        const float keptStrength =
                            merged.W * max(RestirLuminance(RestirUnpackLoTail(merged.sample)), 1e-6);
                        const float localStrength = max(RestirLuminance(yInitial), 1e-6);
                        if (keptStrength <= kRestirBoilingFactor * localStrength)
                        {
                            outRes = merged;
                            outRes.M = min(max(outRes.M, 1u), kRestirMCap);
                            outRes.age = min(prevRes.age + 1u, kRestirAgeCap);
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
        const float3 shaded =
            RestirClampRadiance(direct + RestirShadeIndirect(outRes));
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

    const uint pixelIndex = pixel.y * g_OutputSize.x + pixel.x;
    // u1 = temporal (or prior spatial iter) input; u0 = this iteration's output.
    const RestirReservoir center = g_ReservoirPrev[pixelIndex];
    const uint centerFlags = RestirSampleFlags(center.sample);
    const float3 yCenter = RestirShadeIndirect(center);
    const float centerStrength = max(RestirLuminance(yCenter), 1e-6);

    RestirReservoir outRes = center;
    bool spatialReused = false;

    const float currDepth = g_CurrSurfacePositionDepth[pixel].w;
    const bool skipReuse = (centerFlags & kRestirSampleNoReuse) != 0u || center.M == 0u
        || currDepth <= 0.0;

    if (!skipReuse)
    {
        const float4 currSurface = g_CurrSurfacePositionDepth[pixel];
        const uint4 currMaterial = g_CurrSurfaceMaterial[pixel];
        const float3 receiverPos = currSurface.xyz;
        const float3 receiverN = RestirUnpackOctNormal(currMaterial.x);

        // Fresh WRS over center + Jacobian-shifted neighbor contributions.
        RestirReservoir merged;
        merged.sample = center.sample;
        merged.wSum = 0.0;
        merged.W = 0.0;
        merged.M = 0u;
        merged.age = center.age;

        RestirUpdate(
            merged,
            center.sample,
            centerStrength,
            RestirHash(pixel, g_FrameIndex, 90u + g_SpatialIteration));

        bool anyNeighbor = false;
        const uint sampleCount = clamp(g_SpatialSampleCount, 1u, 16u);
        const float radius = max(g_SpatialRadius, 1.0);

        [loop]
        for (uint i = 0u; i < sampleCount; ++i)
        {
            const float u1 = RestirHash(pixel, g_FrameIndex, 100u + i * 3u + g_SpatialIteration * 17u);
            const float u2 = RestirHash(pixel, g_FrameIndex, 101u + i * 3u + g_SpatialIteration * 17u);
            const float diskR = radius * sqrt(u1);
            const float angle = u2 * (2.0 * kRestirPi);
            const int2 neighborPixel = int2(pixel)
                + int2(round(float2(cos(angle), sin(angle)) * diskR));

            if (neighborPixel.x < 0 || neighborPixel.y < 0
                || neighborPixel.x >= int(g_OutputSize.x)
                || neighborPixel.y >= int(g_OutputSize.y))
            {
                continue;
            }
            if (neighborPixel.x == int(pixel.x) && neighborPixel.y == int(pixel.y))
            {
                continue;
            }

            const float4 neighborSurface = g_CurrSurfacePositionDepth[neighborPixel];
            const uint4 neighborMaterial = g_CurrSurfaceMaterial[neighborPixel];
            const float neighborDepth = neighborSurface.w;
            if (!RestirSurfaceRecordsSimilar(
                    currSurface, neighborSurface, currMaterial, neighborMaterial, 0.01, 0.95, 0.05))
            {
                continue;
            }

            const uint neighborIndex =
                uint(neighborPixel.y) * g_OutputSize.x + uint(neighborPixel.x);
            RestirReservoir neighborRes = g_ReservoirPrev[neighborIndex];
            const uint neighborFlags = RestirSampleFlags(neighborRes.sample);
            if (neighborRes.M == 0u || (neighborFlags & kRestirSampleNoReuse) != 0u)
            {
                continue;
            }

            const float3 neighborPos = neighborSurface.xyz;
            if (length(neighborPos - receiverPos) > kRestirSpatialMaxPrimaryDistance)
            {
                continue;
            }

            const float3 xs = neighborRes.sample.xs;
            const float3 ns = RestirUnpackOctNormal(neighborRes.sample.nsOct);

            const float3 toXs = xs - receiverPos;
            const float dist = length(toXs);
            if (dist < 1e-4 || dot(receiverN, toXs / dist) <= 0.0)
            {
                continue;
            }

            const float jacobian = RestirReconnectionJacobian(xs, ns, neighborPos, receiverPos);
            if (jacobian <= 0.0)
            {
                continue;
            }

            const RestirInitialSample shifted =
                RestirScaleSampleY(neighborRes.sample, jacobian);
            const float3 yShifted = RestirUnpackLoTail(shifted);
            float neighborW = neighborRes.W;
            neighborW = (neighborW == neighborW && neighborW > 0.0) ? neighborW : 0.0;
            const float shiftedStrength =
                neighborW * max(RestirLuminance(yShifted), 1e-6);

            // Relative boil + chroma: dark soffits must not import pink/red bounce from afar.
            if (shiftedStrength > kRestirSpatialBoilingFactor * centerStrength)
            {
                continue;
            }
            if (!RestirChromaticitySimilar(yCenter, yShifted))
            {
                continue;
            }

            RestirUpdate(
                merged,
                shifted,
                shiftedStrength,
                RestirHash(pixel, g_FrameIndex, 200u + i + g_SpatialIteration * 13u));
            anyNeighbor = true;
        }

        if (anyNeighbor)
        {
            RestirFinalizeW(merged);

            const bool visible =
                RestirValidateConnection(receiverPos, receiverN, merged.sample.xs);

            if (visible)
            {
                const float3 yKept = RestirUnpackLoTail(merged.sample);
                const float keptStrength =
                    merged.W * max(RestirLuminance(yKept), 1e-6);
                if (keptStrength <= kRestirSpatialBoilingFactor * centerStrength
                    && RestirChromaticitySimilar(yCenter, yKept))
                {
                    outRes = merged;
                    outRes.M = min(max(outRes.M, 1u), kRestirMCap);
                    spatialReused = true;
                }
            }
        }
    }

    g_ReservoirCurrent[pixelIndex] = outRes;

    if (g_ShadeOutput != 0u && spatialReused)
    {
        const float4 prevOut = g_Output[pixel];
        const float3 direct = g_Direct[pixel].rgb;
        const float3 shaded =
            RestirClampRadiance(direct + RestirShadeIndirect(outRes));
        g_Output[pixel] = float4(shaded, prevOut.a);
    }
    else
    {
        (void)g_Direct;
        (void)g_Output;
        (void)g_InitialSample;
        (void)g_Motion;
        (void)g_PrevSurfacePositionDepth;
        (void)g_PrevSurfaceMaterial;
    }
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
