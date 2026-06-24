Texture2D uDirectLighting : register(t0);
Texture2D uIndirectLighting : register(t1);
Texture2D uDepthMap : register(t2);
Texture2D uSsaoMap : register(t3);
Texture2D uShadowFactorMap : register(t4);

SamplerState uDirectLightingSampler : register(s0);
SamplerState uIndirectLightingSampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uSsaoSampler : register(s3);
SamplerState uShadowFactorSampler : register(s4);

cbuffer PerPixel : register(b0)
{
    int uUseSplitLighting;
    int uUseSsao;
    int uUseShadowFactor;
    float uSsaoPower;
    float uAoStrength;
    int uDebugOcclusionOnly;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float2 uv = input.texCoord;
    float depth = uDepthMap.Sample(uDepthSampler, uv).r;

    float3 direct = 0.0.xxx;
    float3 indirect = 0.0.xxx;

    if (uUseSplitLighting != 0)
    {
        direct = uDirectLighting.Sample(uDirectLightingSampler, uv).rgb;
        indirect = uIndirectLighting.Sample(uIndirectLightingSampler, uv).rgb;
    }
    else
    {
        direct = uDirectLighting.Sample(uDirectLightingSampler, uv).rgb;
    }

    if (uUseShadowFactor != 0)
    {
        direct *= uShadowFactorMap.Sample(uShadowFactorSampler, uv).r;
    }

    if (depth >= 0.9999)
    {
        return float4(direct + indirect, 1.0);
    }

    float indirectOcclusion = 1.0;

    if (uUseSsao != 0)
    {
        float ssao = pow(uSsaoMap.Sample(uSsaoSampler, uv).r, uSsaoPower);
        indirectOcclusion *= lerp(1.0, ssao, uAoStrength);
    }

    if (uDebugOcclusionOnly != 0)
    {
        return float4(indirectOcclusion.xxx, 1.0);
    }

    return float4(direct + indirect * indirectOcclusion, 1.0);
}
