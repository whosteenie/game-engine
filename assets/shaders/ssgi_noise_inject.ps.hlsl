Texture2D uRadianceMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uRadianceSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float uNoiseStrength;
    float uFrameIndex;
    float2 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float3 NoiseRgb(float2 uv, float frame)
{
    const float2 seed0 = uv + float2(frame * 0.753, frame * 0.259);
    const float2 seed1 = uv * 1.37 + float2(frame * 0.419, -frame * 0.611);
    const float2 seed2 = uv * 1.91 - float2(frame * 0.173, frame * 0.887);
    return float3(
        Hash21(seed0) * 2.0 - 1.0,
        Hash21(seed1) * 2.0 - 1.0,
        Hash21(seed2) * 2.0 - 1.0);
}

float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 radiance = uRadianceMap.Sample(uRadianceSampler, input.texCoord);
    if (uNoiseStrength <= 1e-6)
    {
        return radiance;
    }

    const float3 noise = NoiseRgb(input.texCoord, uFrameIndex);
    const float3 noisy = max(radiance.rgb + noise * uNoiseStrength, 0.0.xxx);
    return float4(noisy, radiance.a);
}
