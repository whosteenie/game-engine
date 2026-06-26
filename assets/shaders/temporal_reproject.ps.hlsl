Texture2D uCurrentRadiance : register(t0);
Texture2D uHistoryRadiance : register(t1);
Texture2D uDepth : register(t2);

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
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;

    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 current = uCurrentRadiance.Sample(uCurrentRadianceSampler, uv);
    const float3 currRgb = current.rgb;

    float3 historyRgb = currRgb;
    float historyAccepted = 0.0;

    const float2 clipXY = DepthUvToClipXY(uv);
    float4 worldH = mul(uInvViewProj, float4(clipXY, depth, 1.0));
    worldH.xyz /= worldH.w;

    float4 prevClip = mul(uPrevViewProj, float4(worldH.xyz, 1.0));
    prevClip.xyz /= prevClip.w;
    const float2 historyUv = ClipXYToDepthUv(prevClip.xy);

    if (uHistoryValid > 0.5
        && historyUv.x >= 0.0 && historyUv.x <= 1.0
        && historyUv.y >= 0.0 && historyUv.y <= 1.0)
    {
        historyRgb = uHistoryRadiance.Sample(uHistoryRadianceSampler, historyUv).rgb;
        historyAccepted = 1.0;
    }

    // Neighborhood clamp (variance clipping lite) on current radiance.
    float3 minColor = currRgb;
    float3 maxColor = currRgb;
    const float2 offsets[4] = {
        float2(-texelSize.x, 0.0),
        float2(texelSize.x, 0.0),
        float2(0.0, -texelSize.y),
        float2(0.0, texelSize.y),
    };
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float3 sampleColor = uCurrentRadiance.Sample(uCurrentRadianceSampler, uv + offsets[i]).rgb;
        minColor = min(minColor, sampleColor);
        maxColor = max(maxColor, sampleColor);
    }
    const float3 boxExtent = max(maxColor - currRgb, currRgb - minColor) + 1e-4;
    historyRgb = clamp(historyRgb, currRgb - boxExtent * 1.25, currRgb + boxExtent * 1.25);

    const float3 result = lerp(currRgb, historyRgb, uBlendFactor * historyAccepted);
    return float4(result, historyAccepted);
}
