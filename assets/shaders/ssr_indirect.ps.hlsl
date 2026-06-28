// SSR Phase 4 — replace specular IBL in RT1 with denoised SSR where confidence allows.
// indirect_out = RT1 - spec_ibl + lerp(spec_ibl, ssr.rgb, weight)

#include "screen_space_common.hlsl"

Texture2D uIndirectMap : register(t0);
Texture2D uSsrMap : register(t1);
Texture2D uDepthMap : register(t2);
Texture2D uNormalMap : register(t3);
Texture2D uMaterial0Map : register(t4);
Texture2D uMaterial1Map : register(t5);
TextureCube uPrefilterMap : register(t6);
Texture2D uBrdfLut : register(t7);

SamplerState uIndirectSampler : register(s0);
SamplerState uSsrSampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uNormalSampler : register(s3);
SamplerState uMaterial0Sampler : register(s4);
SamplerState uMaterial1Sampler : register(s5);
SamplerState uPrefilterSampler : register(s6);
SamplerState uBrdfLutSampler : register(s7);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvView;
    float uEnvironmentIntensity;
    float uMaxReflectionLod;
    float uSsrStrength;
    int uDebugSpecReplacement;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 FresnelSchlickRoughness(float cosineTheta, float3 f0, float roughness)
{
    const float3 maxReflection = max(1.0.xxx - roughness, f0);
    return f0 + (maxReflection - f0) * pow(saturate(1.0 - cosineTheta), 5.0);
}

float3 ViewDirectionWorld(float2 texCoord)
{
    const float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewFar = mul(uInvProjection, float4(clipXY, 1.0, 1.0));
    const float3 viewDir = normalize(viewFar.xyz / viewFar.w);
    return normalize(mul((float3x3)uInvView, viewDir));
}

float3 RecomputeSpecularIbl(
    float3 worldNormal,
    float3 viewDir,
    float3 albedo,
    float roughness,
    float metallic)
{
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 reflection = reflect(-viewDir, worldNormal);
    const float3 prefilteredColor = uPrefilterMap.SampleLevel(
        uPrefilterSampler,
        reflection,
        roughness * uMaxReflectionLod).rgb;
    const float nDotV = max(dot(worldNormal, viewDir), 0.0);
    const float2 envBrdf = uBrdfLut.Sample(uBrdfLutSampler, float2(nDotV, roughness)).rg;
    return prefilteredColor * (f0 * envBrdf.x + envBrdf.y) * uEnvironmentIntensity;
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uDepthSampler, uv).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 indirect = uIndirectMap.Sample(uIndirectSampler, uv).rgb;
    const float4 ssr = uSsrMap.Sample(uSsrSampler, uv);
    const float3 worldNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    const float4 material0 = uMaterial0Map.Sample(uMaterial0Sampler, uv);
    const float4 material1 = uMaterial1Map.Sample(uMaterial1Sampler, uv);
    const float3 albedo = material0.rgb;
    const float roughness = material0.a;
    const float metallic = material1.r;

    const float3 viewDir = ViewDirectionWorld(uv);
    const float3 specIbl = RecomputeSpecularIbl(worldNormal, viewDir, albedo, roughness, metallic);
    const float smoothnessWeight = pow(saturate(1.0 - roughness), 2.0);
    const float replacementWeight = saturate(ssr.a * smoothnessWeight * uSsrStrength);

    if (uDebugSpecReplacement != 0)
    {
        return float4(replacementWeight.xxx, 1.0);
    }

    const float3 specFinal = lerp(specIbl, ssr.rgb, replacementWeight);
    return float4(indirect - specIbl + specFinal, 1.0);
}
