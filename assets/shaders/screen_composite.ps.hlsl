#include "environment_sampling.hlsl"

Texture2D uDirectLighting : register(t0);
Texture2D uIndirectLighting : register(t1);
Texture2D uDepthMap : register(t2);
Texture2D uSsaoMap : register(t3);
Texture2D uShadowFactorMap : register(t4);
Texture2D uEquirectMap : register(t5);
Texture2D uSsgiMap : register(t6);

SamplerState uDirectLightingSampler : register(s0);
SamplerState uIndirectLightingSampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uSsaoSampler : register(s3);
SamplerState uShadowFactorSampler : register(s4);
SamplerState uEquirectSampler : register(s5);
SamplerState uSsgiSampler : register(s6);

cbuffer PerPixel : register(b0)
{
    int uUseSplitLighting;
    int uUseSsao;
    int uUseShadowFactor;
    int uUseSsgi;
    float uSsaoPower;
    float uAoStrength;
    float uSsgiStrength;
    int uDebugOcclusionOnly;
    int uBackgroundMode;
    float uSkyboxExposure;
    float uEnvironmentRotationY;
    float3 uSolidBackgroundColor;
    float4x4 uInvProjection;
    float4x4 uInvView;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 ViewRayFromUv(float2 texCoord)
{
    float2 clipXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
    float4 viewFar = mul(uInvProjection, float4(clipXY, 1.0, 1.0));
    float3 viewDir = normalize(viewFar.xyz / viewFar.w);
    return normalize(mul((float3x3)uInvView, viewDir));
}

bool IsBackgroundDepth(float depth)
{
    return depth >= 0.9999;
}

float4 main(PSInput input) : SV_Target
{
    float2 uv = input.texCoord;
    float depth = uDepthMap.Sample(uDepthSampler, uv).r;

    if (IsBackgroundDepth(depth))
    {
        if (uBackgroundMode == 0)
        {
            float3 worldDir = ViewRayFromUv(uv);
            return float4(
                SampleEquirectEnvironment(
                    uEquirectMap,
                    uEquirectSampler,
                    worldDir,
                    uEnvironmentRotationY,
                    uSkyboxExposure),
                1.0);
        }

        return float4(uSolidBackgroundColor, 1.0);
    }

    float3 direct = 0.0.xxx;
    float3 indirect = 0.0.xxx;

    if (uUseSplitLighting != 0)
    {
        float3 fillDirect = uDirectLighting.Sample(uDirectLightingSampler, uv).rgb;
        indirect = uIndirectLighting.Sample(uIndirectLightingSampler, uv).rgb;
        if (uUseSsgi != 0)
        {
            indirect += uSsgiStrength * uSsgiMap.Sample(uSsgiSampler, uv).rgb;
        }
        float4 sunShadow = uShadowFactorMap.Sample(uShadowFactorSampler, uv);
        float3 sunDirect = sunShadow.rgb * (uUseShadowFactor != 0 ? sunShadow.a : 1.0);
        direct = fillDirect + sunDirect;
    }
    else
    {
        direct = uDirectLighting.Sample(uDirectLightingSampler, uv).rgb;
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
