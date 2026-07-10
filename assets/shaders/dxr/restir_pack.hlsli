// Packing helpers for RestirInitialSample / RestirReservoir (G5 / restir-pt.md §2).

#ifndef RESTIR_PACK_HLSLI
#define RESTIR_PACK_HLSLI

#include "restir_types.hlsli"

static const uint kRestirSampleNoReuse = 1u;

float2 RestirOctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    float2 enc = n.xy;
    if (n.z < 0.0)
    {
        const float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        enc = (1.0 - abs(enc.yx)) * signN;
    }
    return enc;
}

uint RestirPackOctNormal(float3 n)
{
    const float2 enc = RestirOctEncode(n);
    return f32tof16(enc.x) | (f32tof16(enc.y) << 16);
}

uint RestirPackHalf2(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

RestirInitialSample RestirMakeInitialSample(
    float3 xs,
    float3 ns,
    float3 loTail,
    float pdf,
    uint seed,
    uint flags)
{
    RestirInitialSample sample;
    sample.xs = xs;
    sample.nsOct = RestirPackOctNormal(ns);
    sample.loTailRg = RestirPackHalf2(loTail.rg);
    sample.loTailBFlags = f32tof16(loTail.b) | (flags << 16);
    sample.pdf = pdf;
    sample.seed = seed;
    return sample;
}

RestirReservoir RestirMakePassthroughReservoir(RestirInitialSample sample)
{
    RestirReservoir reservoir;
    reservoir.sample = sample;
    reservoir.wSum = 1.0;
    reservoir.W = 1.0;
    reservoir.M = 1u;
    reservoir.pad = 0u;
    return reservoir;
}

#endif // RESTIR_PACK_HLSLI
