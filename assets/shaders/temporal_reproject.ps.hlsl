Texture2D uCurrentRadiance : register(t0);
Texture2D uHistoryRadiance : register(t1);
Texture2D uDepth : register(t2);
Texture2D uHistoryDepth : register(t3);

SamplerState uCurrentRadianceSampler : register(s0);
SamplerState uHistoryRadianceSampler : register(s1);
SamplerState uDepthSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvViewProj;
    float4x4 uPrevViewProj;
    float uBlendFactor;
    float uHistoryValid;
    float uTexelSizeX;
    float uTexelSizeY;
    float uDepthRejectThreshold;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ClipXYToDepthUv(float2 clipXY)
{
    return float2(clipXY.x * 0.5 + 0.5, (1.0 - clipXY.y) * 0.5);
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepth.Sample(uDepthSampler, uv).r;

    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 current = uCurrentRadiance.Sample(uCurrentRadianceSampler, uv);

    float4 history = current;
    float historyAccepted = 0.0;

    const float2 clipXY = DepthUvToClipXY(uv);
    float4 worldH = mul(uInvViewProj, float4(clipXY, depth, 1.0));
    if (abs(worldH.w) < 1e-6)
    {
        return current;
    }
    worldH.xyz /= worldH.w;

    float4 prevClip = mul(uPrevViewProj, float4(worldH.xyz, 1.0));
    if (abs(prevClip.w) < 1e-6)
    {
        return current;
    }
    prevClip.xyz /= prevClip.w;
    const float2 historyUv = ClipXYToDepthUv(prevClip.xy);

    if (uHistoryValid > 0.5
        && historyUv.x >= 0.0 && historyUv.x <= 1.0
        && historyUv.y >= 0.0 && historyUv.y <= 1.0)
    {
        const float historyDepth = uHistoryDepth.Sample(uDepthSampler, historyUv).r;
        const float previousDepth = saturate(prevClip.z);
        if (historyDepth < 0.9999 && abs(historyDepth - previousDepth) <= uDepthRejectThreshold)
        {
            history = uHistoryRadiance.Sample(uHistoryRadianceSampler, historyUv);
            historyAccepted = 1.0;
        }
    }

    const float historyBlend = uBlendFactor * historyAccepted;
    const float3 resultRgb = lerp(current.rgb, history.rgb, historyBlend);
    const float resultConfidence = saturate(lerp(current.a, max(current.a, history.a), historyBlend));
    return float4(resultRgb, resultConfidence);
}
