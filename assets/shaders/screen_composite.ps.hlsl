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
    float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;

    float3 direct = 0.0.xxx;
    float3 indirect = 0.0.xxx;

    if (uUseSplitLighting != 0)
    {
        direct = uDirectLighting.Sample(uDirectLightingSampler, input.texCoord).rgb;
        indirect = uIndirectLighting.Sample(uIndirectLightingSampler, input.texCoord).rgb;
    }
    else
    {
        float3 sceneColor = uDirectLighting.Sample(uDirectLightingSampler, input.texCoord).rgb;
        direct = sceneColor;
    }

    if (uUseShadowFactor != 0)
    {
        float shadowFactor = uShadowFactorMap.Sample(uShadowFactorSampler, input.texCoord).r;
        direct *= shadowFactor;
    }

    if (depth >= 0.9999)
    {
        return float4(direct + indirect, 1.0);
    }

    float indirectOcclusion = 1.0;

    if (uUseSsao != 0)
    {
        float ssao = pow(uSsaoMap.Sample(uSsaoSampler, input.texCoord).r, uSsaoPower);
        indirectOcclusion *= lerp(1.0, ssao, uAoStrength);
    }

    if (uDebugOcclusionOnly != 0)
    {
        return float4(indirectOcclusion.xxx, 1.0);
    }

    return float4(direct + indirect * indirectOcclusion, 1.0);
}
