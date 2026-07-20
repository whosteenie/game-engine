Texture2D uCurrentBloom : register(t0);
Texture2D uHistoryBloom : register(t1);
Texture2D uVelocity : register(t2);
Texture2D uDepth : register(t3);

SamplerState uCurrentBloomSampler : register(s0);
SamplerState uHistoryBloomSampler : register(s1);
SamplerState uVelocitySampler : register(s2);
SamplerState uDepthSampler : register(s3);

cbuffer PerPixel : register(b0)
{
    float uBlendFactor;
    float uSameUvBlendFactor;
    float uHistoryValid;
    float uDepthThreshold;
    float uTexelSizeX;
    float uTexelSizeY;
    float uWarmupFactor;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float2 VelocityNdcToUvDelta(float2 velocityNdc)
{
    return float2(velocityNdc.x * 0.5, -velocityNdc.y * 0.5);
}

float BloomMax(float3 rgb)
{
    return max(rgb.r, max(rgb.g, rgb.b));
}

float3 ClipHistory(float3 historyRgb, float2 uv, float2 texelSize)
{
    const float3 currentRgb = uCurrentBloom.Sample(uCurrentBloomSampler, uv).rgb;
    float3 minRgb = currentRgb;
    float3 maxRgb = currentRgb;
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
        const float3 sampleRgb = uCurrentBloom.Sample(uCurrentBloomSampler, uv + offsets[i]).rgb;
        minRgb = min(minRgb, sampleRgb);
        maxRgb = max(maxRgb, sampleRgb);
    }

    const float3 extent = max(maxRgb - currentRgb, currentRgb - minRgb) + 1e-4;
    return clamp(historyRgb, currentRgb - extent * 1.25, currentRgb + extent * 1.25);
}

float ComputeGhostRejectConfidence(float3 current, float3 history)
{
    const float currentMax = BloomMax(current);
    const float historyMax = BloomMax(history);
    return saturate((currentMax + 0.0015) / (historyMax + 0.0015));
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;
    const float3 current = uCurrentBloom.Sample(uCurrentBloomSampler, uv).rgb;

    if (depth >= 0.9999 || uHistoryValid <= 0.5)
    {
        return float4(current, depth);
    }

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    const float motion = length(velocityNdc);
    const float2 historyUv = uv - VelocityNdcToUvDelta(velocityNdc);
    if (historyUv.x < 0.0 || historyUv.x > 1.0
        || historyUv.y < 0.0 || historyUv.y > 1.0)
    {
        // A failed moving reprojection may not fall back to same-UV history.
        return float4(current, depth);
    }

    // History alpha owns the previous depth in the same guide domain as bloom motion.
    const float4 historySample = uHistoryBloom.Sample(uHistoryBloomSampler, historyUv);
    const float previousDepth = historySample.a;
    const bool skyMismatch = (depth >= 0.9999) != (previousDepth >= 0.9999);
    const float depthDelta = abs(previousDepth - depth);
    if (skyMismatch || depthDelta >= uDepthThreshold)
    {
        return float4(current, depth);
    }

    const float depthCoherent = 1.0
        - saturate(depthDelta / max(uDepthThreshold, 1e-5));
    const float3 history = ClipHistory(historySample.rgb, uv, texelSize);
    const float confidence = ComputeGhostRejectConfidence(current, history);
    if (confidence < 0.12)
    {
        return float4(current, depth);
    }

    float3 result = current;
    if (motion > 1e-6)
    {
        const float velocityAccepted = saturate(motion * 48.0) * depthCoherent;
        result = lerp(current, history, uBlendFactor * velocityAccepted * confidence);
    }
    else
    {
        const float staticWeight = saturate(1.0 - motion * 72.0);
        const float blend = uSameUvBlendFactor * staticWeight * uWarmupFactor;
        result = max(current, lerp(current, history, blend));
    }

    return float4(result, depth);
}
