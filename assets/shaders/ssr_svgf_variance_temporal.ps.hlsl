// SVGF pass 2 — temporal variance accumulation for SSR.
// Spatial variance from raw trace + history reprojection; boosted by temporal color delta.

#include "screen_space_common.hlsl"
#include "ssr_svgf_common.hlsl"

Texture2D uCurrentTrace : register(t0);
Texture2D uFilteredColor : register(t1);
Texture2D uHistoryVariance : register(t2);
Texture2D uVelocity : register(t3);
Texture2D uDepth : register(t4);

SamplerState uCurrentTraceSampler : register(s0);
SamplerState uFilteredColorSampler : register(s1);
SamplerState uHistoryVarianceSampler : register(s2);
SamplerState uVelocitySampler : register(s3);
SamplerState uDepthSampler : register(s4);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvViewProj;
    float4x4 uPrevViewProj;
    int uUseMotionVectors;
    float uBlendFactor;
    float uHistoryValid;
    float uDepthThreshold;
    float uTexelSizeX;
    float uTexelSizeY;
    float2 _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float SampleHistoryVariance(float2 historyUv, float historyAccepted)
{
    if (historyAccepted <= 0.0)
    {
        return 0.0;
    }

    return max(uHistoryVariance.Sample(uHistoryVarianceSampler, historyUv).r, 0.0);
}

float ResolveHistoryUvMotion(float2 uv, float depth, out float historyAccepted)
{
    historyAccepted = 0.0;

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    const float motion = length(velocityNdc);
    const float2 historyUv = uv - SsrVelocityNdcToUvDelta(velocityNdc);

    if (motion <= 1e-6
        || historyUv.x < 0.0 || historyUv.x > 1.0
        || historyUv.y < 0.0 || historyUv.y > 1.0)
    {
        return uv;
    }

    const float depthAtHistoryUv = uDepth.Sample(uDepthSampler, historyUv).r;
    const float depthDelta = depthAtHistoryUv - depth;
    const float disoccluded = step(uDepthThreshold, depthDelta);
    const float depthCoherent =
        (1.0 - disoccluded) * (1.0 - saturate(abs(depthDelta) / max(uDepthThreshold, 1e-5)));

    historyAccepted = saturate(motion * 40.0) * depthCoherent;
    return historyUv;
}

float ResolveHistoryUvWorld(float2 uv, float depth, out float historyAccepted)
{
    historyAccepted = 0.0;

    const float2 clipXY = DepthUvToClipXY(uv);
    float4 worldH = mul(uInvViewProj, float4(clipXY, depth, 1.0));
    if (abs(worldH.w) < 1e-6)
    {
        return uv;
    }
    worldH.xyz /= worldH.w;

    float4 prevClip = mul(uPrevViewProj, float4(worldH.xyz, 1.0));
    if (abs(prevClip.w) < 1e-6)
    {
        return uv;
    }
    prevClip.xyz /= prevClip.w;
    const float2 historyUv = ClipXYToDepthUv(prevClip.xy);

    if (historyUv.x < 0.0 || historyUv.x > 1.0 || historyUv.y < 0.0 || historyUv.y > 1.0)
    {
        return uv;
    }

    const float historyDepth = uDepth.Sample(uDepthSampler, historyUv).r;
    const float previousDepth = saturate(prevClip.z);
    if (historyDepth >= 0.9999 || abs(historyDepth - previousDepth) > uDepthThreshold)
    {
        return uv;
    }

    historyAccepted = 1.0;
    return historyUv;
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;

    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 current = uCurrentTrace.Sample(uCurrentTraceSampler, uv);
    const float4 filtered = uFilteredColor.Sample(uFilteredColorSampler, uv);
    const float spatialVariance = SsrSpatialLumaVariance(uCurrentTrace, uCurrentTraceSampler, uv, texelSize);

    if (uHistoryValid <= 0.5)
    {
        const float currentLuma = SsrTraceLuma(current);
        const float filteredLuma = SsrTraceLuma(filtered);
        const float deltaVariance = (currentLuma - filteredLuma) * (currentLuma - filteredLuma);
        return float4(max(spatialVariance, deltaVariance), 0.0, 0.0, 0.0);
    }

    float historyAccepted = 0.0;
    float2 historyUv = uv;
    if (uUseMotionVectors != 0)
    {
        historyUv = ResolveHistoryUvMotion(uv, depth, historyAccepted);
        if (historyAccepted <= 0.0)
        {
            const float staticWeight = saturate(1.0 - length(uVelocity.Sample(uVelocitySampler, uv).rg) * 64.0);
            historyAccepted = staticWeight;
            historyUv = uv;
        }
    }
    else
    {
        historyUv = ResolveHistoryUvWorld(uv, depth, historyAccepted);
    }

    const float historyVariance = SampleHistoryVariance(historyUv, historyAccepted);
    const float temporalBlend = uBlendFactor * historyAccepted;
    float variance = lerp(spatialVariance, historyVariance, temporalBlend);

    const float currentLuma = SsrTraceLuma(current);
    const float filteredLuma = SsrTraceLuma(filtered);
    const float deltaVariance = (currentLuma - filteredLuma) * (currentLuma - filteredLuma);
    variance = max(variance, deltaVariance);

    return float4(max(variance, 1e-8), 0.0, 0.0, 0.0);
}
