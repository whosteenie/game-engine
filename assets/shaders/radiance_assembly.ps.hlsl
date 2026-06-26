// SSGI Phase 3 — diffuse-dominant radiance assembly for screen-space trace hits.
//
//   radiance.rgb = emissive
//                + diffuse_dominant_indirect
//                + optional fill_direct_term
//   radiance.a   = validity (1 = geometry, 0 = sky / background)
//
// Indirect comes from split RT1 (IBL ambient). Specular IBL is attenuated by roughness/metal.
// Sun direct is excluded (lives in RT3). Fill direct (RT0) is optional, diffuse-weighted.

Texture2D uDirectLighting : register(t0);
Texture2D uIndirectLighting : register(t1);
Texture2D uDepthMap : register(t2);
Texture2D uMaterial0Map : register(t3);
Texture2D uMaterial1Map : register(t4);

SamplerState uDirectLightingSampler : register(s0);
SamplerState uIndirectLightingSampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uMaterial0Sampler : register(s3);
SamplerState uMaterial1Sampler : register(s4);

cbuffer PerPixel : register(b0)
{
    int uIncludeFillDirect;
    float3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

bool IsBackgroundDepth(float depth)
{
    return depth >= 0.9999;
}

float3 StripSpecularIblLeak(float3 indirect, float roughness, float metallic)
{
    const float specLeak = saturate((1.0 - roughness) * (0.35 + 0.65 * metallic));
    return indirect * (1.0 - specLeak);
}

float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (IsBackgroundDepth(depth))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 material0 = uMaterial0Map.Sample(uMaterial0Sampler, input.texCoord);
    const float4 material1 = uMaterial1Map.Sample(uMaterial1Sampler, input.texCoord);

    const float3 albedo = material0.rgb;
    const float roughness = material0.a;
    const float metallic = material1.r;
    const float3 emissive = material1.gba;

    const float3 indirect = uIndirectLighting.Sample(uIndirectLightingSampler, input.texCoord).rgb;
    const float3 diffuseIndirect = StripSpecularIblLeak(indirect, roughness, metallic);

    float3 radiance = emissive + diffuseIndirect;

    if (uIncludeFillDirect != 0)
    {
        const float3 directFill = uDirectLighting.Sample(uDirectLightingSampler, input.texCoord).rgb;
        const float diffuseFillWeight = (1.0 - metallic) * saturate(roughness);
        radiance += directFill * diffuseFillWeight;
    }

    return float4(max(radiance, 0.0.xxx), 1.0);
}
