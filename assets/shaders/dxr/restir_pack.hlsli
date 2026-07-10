// Packing / WRS helpers for RestirInitialSample / RestirReservoir (G5/R2 / restir-pt.md).

#ifndef RESTIR_PACK_HLSLI
#define RESTIR_PACK_HLSLI

#include "restir_types.hlsli"

static const uint kRestirSampleNoReuse = 1u;
static const uint kRestirMCap = 20u;
static const float kRestirBoilingFactor = 10.0;

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

float3 RestirOctDecode(float2 enc)
{
    float3 n = float3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    if (n.z < 0.0)
    {
        const float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * signN;
    }
    return normalize(n);
}

uint RestirPackOctNormal(float3 n)
{
    const float2 enc = RestirOctEncode(n);
    return f32tof16(enc.x) | (f32tof16(enc.y) << 16);
}

float3 RestirUnpackOctNormal(uint packed)
{
    const float2 enc = float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
    return RestirOctDecode(enc);
}

uint RestirPackHalf2(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

float2 RestirUnpackHalf2(uint packed)
{
    return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
}

float3 RestirUnpackLoTail(RestirInitialSample sample)
{
    return float3(
        RestirUnpackHalf2(sample.loTailRg),
        f16tof32(sample.loTailBFlags & 0xffffu));
}

uint RestirSampleFlags(RestirInitialSample sample)
{
    return sample.loTailBFlags >> 16;
}

float RestirLuminance(float3 c)
{
    return max(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b, 0.0);
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
    // RIS: p̂ ≈ lum(Y) with Y = t1·Lo_tail already stored in sample; W=1 ⇒ shade = Y.
    const float pHat = max(RestirLuminance(RestirUnpackLoTail(sample)), 1e-6);
    reservoir.wSum = pHat;
    reservoir.W = 1.0;
    reservoir.M = 1u;
    reservoir.pad = 0u;
    return reservoir;
}

void RestirFinalizeW(inout RestirReservoir reservoir)
{
    const float pHat = max(RestirLuminance(RestirUnpackLoTail(reservoir.sample)), 1e-6);
    const float m = max(float(reservoir.M), 1.0);
    reservoir.W = reservoir.wSum / (m * pHat);
}

// Stream one candidate into the reservoir (weighted reservoir sampling).
void RestirUpdate(
    inout RestirReservoir reservoir,
    RestirInitialSample candidate,
    float weight,
    float xi)
{
    if (weight <= 0.0)
    {
        return;
    }

    reservoir.wSum += weight;
    reservoir.M += 1u;
    if (reservoir.wSum <= 0.0 || xi * reservoir.wSum < weight)
    {
        reservoir.sample = candidate;
    }
}

// Merge another reservoir as M samples with pairwise-MIS scale on its streamed weight.
void RestirMergeReservoir(
    inout RestirReservoir dst,
    RestirReservoir src,
    float misWeight,
    float xi)
{
    if (src.M == 0u || misWeight <= 0.0)
    {
        return;
    }

    const float pHat = max(RestirLuminance(RestirUnpackLoTail(src.sample)), 1e-6);
    const float weight = misWeight * src.W * pHat * float(src.M);
    if (weight <= 0.0)
    {
        return;
    }

    dst.wSum += weight;
    dst.M += src.M;
    if (dst.wSum <= 0.0 || xi * dst.wSum < weight)
    {
        dst.sample = src.sample;
    }
}

float3 RestirShadeIndirect(RestirReservoir reservoir)
{
    const float3 y = RestirUnpackLoTail(reservoir.sample);
    return y * reservoir.W;
}

// Match path_tracer / hit_shading ClampRadiance (kMaxRadiance = 64).
float3 RestirClampRadiance(float3 radiance)
{
    radiance.x = (radiance.x == radiance.x) ? radiance.x : 0.0;
    radiance.y = (radiance.y == radiance.y) ? radiance.y : 0.0;
    radiance.z = (radiance.z == radiance.z) ? radiance.z : 0.0;
    radiance = clamp(radiance, 0.0.xxx, 65504.0.xxx);
    const float luminance = RestirLuminance(radiance);
    const float kMaxRadiance = 64.0;
    if (luminance <= kMaxRadiance)
    {
        return radiance;
    }
    return radiance * (kMaxRadiance / max(luminance, 1e-4));
}

#endif // RESTIR_PACK_HLSLI
