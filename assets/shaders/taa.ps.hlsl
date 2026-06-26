Texture2D uColor : register(t0);
Texture2D uHistory : register(t1);
Texture2D uDepth : register(t2);
Texture2D uVelocity : register(t3);

SamplerState uColorSampler : register(s0);
SamplerState uHistorySampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uVelocitySampler : register(s3);

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

// Motion vectors are stored as currentNDC - previousNDC (unjittered).
float2 VelocityNdcToUvDelta(float2 velocityNdc)
{
    return float2(velocityNdc.x * 0.5, -velocityNdc.y * 0.5);
}

float3 RGBToYCoCg(float3 rgb)
{
    const float Y = dot(rgb, float3(0.25, 0.5, 0.25));
    const float Co = dot(rgb, float3(0.5, 0.0, -0.5));
    const float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    const float Y = ycocg.x;
    const float Co = ycocg.y;
    const float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float3 ClipHistoryYCoCgAtUv(float3 historyRgb, float2 uv, float2 texelSize)
{
    const float3 currentRgb = uColor.Sample(uColorSampler, uv).rgb;
    float3 minYCoCg = RGBToYCoCg(currentRgb);
    float3 maxYCoCg = minYCoCg;
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
        const float3 sampleRgb = uColor.Sample(uColorSampler, uv + offsets[i]).rgb;
        const float3 sampleYCoCg = RGBToYCoCg(sampleRgb);
        minYCoCg = min(minYCoCg, sampleYCoCg);
        maxYCoCg = max(maxYCoCg, sampleYCoCg);
    }

    const float3 extent = max(maxYCoCg - RGBToYCoCg(currentRgb), RGBToYCoCg(currentRgb) - minYCoCg) + 1e-4;
    const float3 historyYCoCg = RGBToYCoCg(historyRgb);
    const float3 clippedYCoCg = clamp(historyYCoCg, RGBToYCoCg(currentRgb) - extent * 1.25, RGBToYCoCg(currentRgb) + extent * 1.25);
    return YCoCgToRGB(clippedYCoCg);
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float depth = uDepth.Sample(uDepthSampler, uv).r;
    const float3 current = uColor.Sample(uColorSampler, uv).rgb;

    if (depth >= 0.9999)
    {
        return float4(current, 1.0);
    }

    float2 historyUv = uv;
    float historyAccepted = 0.0;

    const float2 velocityNdc = uVelocity.Sample(uVelocitySampler, uv).rg;
    if (length(velocityNdc) > 1e-6)
    {
        historyUv = uv - VelocityNdcToUvDelta(velocityNdc);
        if (historyUv.x >= 0.0 && historyUv.x <= 1.0 && historyUv.y >= 0.0 && historyUv.y <= 1.0)
        {
            historyAccepted = 1.0;
        }
    }
    else if (uHistoryValid > 0.5)
    {
        const float2 clipXY = DepthUvToClipXY(uv);
        float4 worldH = mul(uInvViewProj, float4(clipXY, depth, 1.0));
        worldH.xyz /= worldH.w;

        float4 prevClip = mul(uPrevViewProj, float4(worldH.xyz, 1.0));
        prevClip.xyz /= prevClip.w;
        historyUv = ClipXYToDepthUv(prevClip.xy);

        if (historyUv.x >= 0.0 && historyUv.x <= 1.0 && historyUv.y >= 0.0 && historyUv.y <= 1.0)
        {
            historyAccepted = 1.0;
        }
    }

    float3 history = current;
    if (historyAccepted > 0.5)
    {
        history = uHistory.Sample(uHistorySampler, historyUv).rgb;
        history = ClipHistoryYCoCgAtUv(history, uv, texelSize);
    }

    const float3 result = lerp(current, history, uBlendFactor * historyAccepted);
    return float4(result, 1.0);
}
