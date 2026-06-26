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
    float uHistoryValid;
    float uTexelSizeX;
    float uTexelSizeY;
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

float3 ClipHistory(float3 historyRgb, float2 uv, float2 texelSize)
{
    const float3 currentRgb = uCurrentBloom.Sample(uCurrentBloomSampler, uv).rgb;
    float3 minRgb = currentRgb;
    float3 maxRgb = currentRgb;
    const float2 offsets[4] = {
        float2(-texelSize.x, 0.0),
        float2(texelSize.x, 0.0),
        float2(0.0, -texelSize.y),
        float2(0.0, texelSize.y),
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float3 sampleRgb = uCurrentBloom.Sample(uCurrentBloomSampler, uv + offsets[i]).rgb;
        minRgb = min(minRgb, sampleRgb);
        maxRgb = max(maxRgb, sampleRgb);
    }

    const float3 extent = max(maxRgb - currentRgb, currentRgb - minRgb) + 1e-4;
    return clamp(historyRgb, currentRgb - extent * 1.5, currentRgb + extent * 1.5);
}

float3 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;
    const float3 current = uCurrentBloom.Sample(uCurrentBloomSampler, uv).rgb;

    if (depth >= 0.9999)
    {
        return current;
    }

    float historyAccepted = 0.0;
    float3 history = current;

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    const float2 historyUv = uv - VelocityNdcToUvDelta(velocityNdc);
    if (uHistoryValid > 0.5
        && length(velocityNdc) > 1e-6
        && historyUv.x >= 0.0 && historyUv.x <= 1.0
        && historyUv.y >= 0.0 && historyUv.y <= 1.0)
    {
        history = uHistoryBloom.Sample(uHistoryBloomSampler, historyUv).rgb;
        history = ClipHistory(history, uv, texelSize);
        historyAccepted = 1.0;
    }

    return lerp(current, history, uBlendFactor * historyAccepted);
}
