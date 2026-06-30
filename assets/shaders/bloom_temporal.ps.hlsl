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

// Reject bright bloom history on pixels that are now dark (occlusion / parallax reveal).
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
        return float4(current, 1.0);
    }

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    const float motion = length(velocityNdc);
    const float staticWeight = saturate(1.0 - motion * 72.0);
    const float movingWeight = saturate(motion * 120.0);

    const float3 historySameUv = ClipHistory(
        uHistoryBloom.Sample(uHistoryBloomSampler, uv).rgb,
        uv,
        texelSize);

    float velocityAccepted = 0.0;
    float depthCoherent = 0.0;
    float3 historyVelocity = current;

    const float2 historyUv = uv - VelocityNdcToUvDelta(velocityNdc);
    if (motion > 1e-6
        && historyUv.x >= 0.0 && historyUv.x <= 1.0
        && historyUv.y >= 0.0 && historyUv.y <= 1.0)
    {
        const float depthAtHistoryUv = uDepth.Sample(uDepthSampler, historyUv).r;
        const float depthDelta = depthAtHistoryUv - depth;

        // New closer surface at this pixel — do not pull history forward.
        const float disoccluded = step(uDepthThreshold, depthDelta);
        depthCoherent = (1.0 - disoccluded)
            * (1.0 - saturate(abs(depthDelta) / max(uDepthThreshold, 1e-5)));

        if (depthCoherent > 0.01)
        {
            historyVelocity = ClipHistory(
                uHistoryBloom.Sample(uHistoryBloomSampler, historyUv).rgb,
                uv,
                texelSize);
            velocityAccepted = saturate(motion * 48.0) * depthCoherent;
        }
    }

    float3 result = current;

    if (velocityAccepted > 0.01)
    {
        const float confidence = ComputeGhostRejectConfidence(current, historyVelocity);
        if (confidence >= 0.12)
        {
            result = lerp(current, historyVelocity, uBlendFactor * velocityAccepted * confidence);
        }
    }
    else if (staticWeight > 0.05)
    {
        const float blend = uSameUvBlendFactor * staticWeight * uWarmupFactor;
        const float3 blended = lerp(current, historySameUv, blend);

        // Never let temporal filtering suppress this frame's spatial bloom halo.
        if (movingWeight > 0.2)
        {
            const float confidence = ComputeGhostRejectConfidence(current, historySameUv);
            if (confidence >= 0.12)
            {
                result = max(current, lerp(current, historySameUv, blend * confidence));
            }
        }
        else
        {
            result = max(current, blended);
        }
    }

    return float4(result, 1.0);
}
