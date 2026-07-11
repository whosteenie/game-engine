Texture2D<float4> uStatsMap : register(t0);
SamplerState uStatsSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uDebugMode;
    float uDeltaGain;
    float uRelativeSigmaGain;
    float _Padding0;
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
    const float count = max(stats.a, 1.0);
    const float mean = stats.r;
    const float variance = count > 1.0 ? max(stats.g / (count - 1.0), 0.0) : 0.0;

    if (uDebugMode == 1)
    {
        return float4(Heat(stats.b * uDeltaGain), 1.0);
    }

    const float relativeSigma = sqrt(variance) / max(abs(mean), 0.001);
    return float4(Heat(relativeSigma * uRelativeSigmaGain), 1.0);
}
