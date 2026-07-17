Texture2D<float4> uStatsMap : register(t0);
SamplerState uStatsSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uDebugMode;
    float uDeltaGain;
    float uRelativeSigmaGain;
    float _Padding0;
    float2 uRoiMin;
    float2 uRoiMax;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 Heat(float value)
{
    const float v = saturate(value);
    return saturate(float3(1.5 * v - 0.25, 1.5 - abs(v - 0.5) * 3.0, 1.25 - 1.5 * v));
}

float4 main(PSInput input) : SV_Target
{
    const float4 stats = uStatsMap.Sample(uStatsSampler, input.texCoord);
    if (stats.a <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float count = max(stats.a, 1.0);
    const float mean = stats.r;
    const float variance = count > 1.0 ? max(stats.g / (count - 1.0), 0.0) : 0.0;

    if (uDebugMode == 1)
    {
        const float3 color = Heat(max(stats.b, 0.0) * uDeltaGain);
        const float2 edgeDistance = min(abs(input.texCoord - uRoiMin), abs(input.texCoord - uRoiMax));
        const bool onRoiBorder = all(input.texCoord >= uRoiMin) && all(input.texCoord <= uRoiMax)
            && (edgeDistance.x < 0.002 || edgeDistance.y < 0.002);
        return float4(onRoiBorder ? float3(0.1, 1.0, 1.0) : color, 1.0);
    }

    const float relativeSigma = sqrt(variance) / max(abs(mean), 0.001);
    const float3 color = Heat(relativeSigma * uRelativeSigmaGain);
    const float2 edgeDistance = min(abs(input.texCoord - uRoiMin), abs(input.texCoord - uRoiMax));
    const bool onRoiBorder = all(input.texCoord >= uRoiMin) && all(input.texCoord <= uRoiMax)
        && (edgeDistance.x < 0.002 || edgeDistance.y < 0.002);
    return float4(onRoiBorder ? float3(0.1, 1.0, 1.0) : color, 1.0);
}
