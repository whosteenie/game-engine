Texture2D uTemporalRadiance : register(t0);
Texture2D uCurrentRadiance : register(t1);
Texture2D uDepthMap : register(t2);

SamplerState uTemporalRadianceSampler : register(s0);
SamplerState uCurrentRadianceSampler : register(s1);
SamplerState uDepthSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    int uGiTemporalDebugMode;
    float uDifferenceGain;
    float2 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// 0 = accumulated radiance rgb, 1 = disocclusion, 2 = temporal delta (amplified)
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 temporal = uTemporalRadiance.Sample(uTemporalRadianceSampler, input.texCoord);
    if (uGiTemporalDebugMode == 0)
    {
        return float4(temporal.rgb, 1.0);
    }

    if (uGiTemporalDebugMode == 1)
    {
        const float accepted = temporal.a;
        return float4(1.0 - accepted, accepted, 0.0, 1.0);
    }

    const float3 current = uCurrentRadiance.Sample(uCurrentRadianceSampler, input.texCoord).rgb;
    const float3 delta = abs(temporal.rgb - current) * uDifferenceGain;
    return float4(delta, 1.0);
}
