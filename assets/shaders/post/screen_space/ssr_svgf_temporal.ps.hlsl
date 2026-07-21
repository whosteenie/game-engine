// SVGF pass 1 — temporal color accumulation for SSR (motion-vector reprojection).
// Neighborhood clamp + luminance agreement; separate variance pass follows.

#include "../../common/screen_space_common.hlsl"
#include "ssr_svgf_common.hlsl"

Texture2D uCurrentTrace : register(t0);
Texture2D uHistoryTrace : register(t1);
Texture2D uVelocity : register(t2);
Texture2D uDepth : register(t3);
Texture2D uNormalMap : register(t4);
Texture2D uHistoryDepth : register(t5); // last frame's device depth (gi_depth_history)

SamplerState uCurrentTraceSampler : register(s0);
SamplerState uHistoryTraceSampler : register(s1);
SamplerState uVelocitySampler : register(s2);
SamplerState uDepthSampler : register(s3);
SamplerState uNormalSampler : register(s4);
SamplerState uHistoryDepthSampler : register(s5);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvViewProj;   // current frame, unjittered
    float4x4 uPrevViewProj;  // previous frame, unjittered
    float uBlendFactor;
    float uSameUvBlendFactor;
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

float ReceiverEdgeFactor(float2 uv, float2 texelSize)
{
    const float3 centerNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    const float centerDepth = ViewDepthAt(uDepth, uDepthSampler, uInvProjection, uv);

    float maxNormalDelta = 0.0;
    float maxDepthDelta = 0.0;
    const float2 offsets[4] = {
        float2(texelSize.x, 0.0),
        float2(-texelSize.x, 0.0),
        float2(0.0, texelSize.y),
        float2(0.0, -texelSize.y),
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float2 sampleUv = uv + offsets[i];
        const float3 sampleNormal = normalize(uNormalMap.Sample(uNormalSampler, sampleUv).rgb);
        maxNormalDelta = max(maxNormalDelta, 1.0 - saturate(dot(centerNormal, sampleNormal)));

        const float sampleDepth = ViewDepthAt(uDepth, uDepthSampler, uInvProjection, sampleUv);
        maxDepthDelta = max(
            maxDepthDelta,
            abs(sampleDepth - centerDepth) / max(abs(centerDepth), 1e-3));
    }

    const float normalEdge = smoothstep(0.02, 0.12, maxNormalDelta);
    const float depthEdge = smoothstep(0.015, 0.08, maxDepthDelta);
    return 1.0 - max(normalEdge, depthEdge);
}

float4 ClipHistory(float4 history, float2 uv, float2 texelSize, float clampScale)
{
    const float4 current = uCurrentTrace.Sample(uCurrentTraceSampler, uv);
    float3 minRgb = current.rgb;
    float3 maxRgb = current.rgb;
    float minAlpha = current.a;
    float maxAlpha = current.a;
    const float2 offsets[8] = {
        float2(-texelSize.x, 0.0),
        float2(texelSize.x, 0.0),
        float2(0.0, -texelSize.y),
        float2(0.0, texelSize.y),
        float2(-texelSize.x, -texelSize.y),
        float2(texelSize.x, -texelSize.y),
        float2(-texelSize.x, texelSize.y),
        float2(texelSize.x, texelSize.y),
    };

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const float4 sampleTrace = uCurrentTrace.Sample(uCurrentTraceSampler, uv + offsets[i]);
        minRgb = min(minRgb, sampleTrace.rgb);
        maxRgb = max(maxRgb, sampleTrace.rgb);
        minAlpha = min(minAlpha, sampleTrace.a);
        maxAlpha = max(maxAlpha, sampleTrace.a);
    }

    const float3 rgbExtent = max(maxRgb - current.rgb, current.rgb - minRgb) + 1e-4;
    const float alphaExtent = max(maxAlpha - current.a, current.a - minAlpha) + 1e-4;
    history.rgb = clamp(history.rgb, current.rgb - rgbExtent * clampScale, current.rgb + rgbExtent * clampScale);
    history.a = clamp(history.a, current.a - alphaExtent * clampScale, current.a + alphaExtent * clampScale);
    return history;
}

// SSR-06: the old LuminanceAgreement/GhostRejectConfidence gates collapsed the history blend
// whenever current != history — but a stochastic 1-2 spp trace ALWAYS disagrees frame to
// frame, so accumulation died exactly where it was needed and the output shimmered.
// Standard SVGF order: accumulate first (high blend factor), bound ghosting with the
// neighborhood clamp (ClipHistory above), reject only on disocclusion (depth test below).

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;
    const float4 current = uCurrentTrace.Sample(uCurrentTraceSampler, uv);

    if (depth >= 0.9999 || uHistoryValid <= 0.5)
    {
        return current;
    }

    const float receiverEdge = ReceiverEdgeFactor(uv, texelSize);
    const float clampScale = lerp(0.85, 1.12, receiverEdge);

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    const float motion = length(velocityNdc);
    const float staticWeight = saturate(1.0 - motion * 64.0);

    const float4 historySameUv = ClipHistory(
        uHistoryTrace.Sample(uHistoryTraceSampler, uv),
        uv,
        texelSize,
        clampScale);

    float velocityAccepted = 0.0;
    float depthCoherent = 0.0;
    float4 historyVelocity = current;

    const float2 historyUv = uv - SsrVelocityNdcToUvDelta(velocityNdc);
    if (motion > 1e-6
        && historyUv.x >= 0.0 && historyUv.x <= 1.0
        && historyUv.y >= 0.0 && historyUv.y <= 1.0)
    {
        // SSR-06 follow-up: test disocclusion against LAST frame's depth, not the current
        // depth buffer sampled at the reprojected UV (which mis-rejects under any camera
        // motion). Reproject this pixel into the previous frame and compare its expected
        // device depth with what the previous frame actually stored there.
        const float2 clipXY = DepthUvToClipXY(uv);
        const float4 worldH = mul(uInvViewProj, float4(clipXY, depth, 1.0));
        const float3 worldPos = worldH.xyz / worldH.w;
        const float4 prevClip = mul(uPrevViewProj, float4(worldPos, 1.0));
        const float expectedPrevDepth = prevClip.z / max(prevClip.w, 1e-6);
        const float historyDepth = uHistoryDepth.Sample(uHistoryDepthSampler, historyUv).r;
        const float depthDelta = abs(expectedPrevDepth - historyDepth);
        depthCoherent = 1.0 - saturate(depthDelta / max(uDepthThreshold, 1e-5));

        if (depthCoherent > 0.01)
        {
            historyVelocity = ClipHistory(
                uHistoryTrace.Sample(uHistoryTraceSampler, historyUv),
                uv,
                texelSize,
                clampScale);
            velocityAccepted = saturate(motion * 40.0) * depthCoherent;
        }
    }

    float4 result = current;

    if (velocityAccepted > 0.01)
    {
        const float edgeBlend = lerp(0.35, 1.0, receiverEdge);
        const float blend = uBlendFactor * velocityAccepted * edgeBlend;
        result.rgb = lerp(current.rgb, historyVelocity.rgb, blend);
        result.a = lerp(current.a, historyVelocity.a, blend);
    }
    else if (staticWeight > 0.05)
    {
        const float edgeBlend = lerp(0.45, 1.0, receiverEdge);
        const float effectiveBlend = uSameUvBlendFactor * staticWeight * edgeBlend;
        result.rgb = lerp(current.rgb, historySameUv.rgb, effectiveBlend);
        result.a = lerp(current.a, historySameUv.a, effectiveBlend);
    }

    return result;
}

