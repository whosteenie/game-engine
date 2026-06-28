// SSR Phase 1 — specular scene-color assembly for screen-space reflection trace hits.
//
//   sceneColor.rgb = fill_direct + emissive (RT0) + sun_direct × shadow (RT3)
//   sceneColor.a   = validity (1 = geometry, 0 = sky / background)
//
// Linear HDR, pre-tonemap. Do NOT use radiance_assembly.ps.hlsl for SSR — that buffer is
// diffuse-dominant SSGI input (strips specular IBL, excludes sun direct).

Texture2D uDirectLighting : register(t0);
Texture2D uSunShadowMap : register(t1);
Texture2D uDepthMap : register(t2);

SamplerState uDirectLightingSampler : register(s0);
SamplerState uSunShadowSampler : register(s1);
SamplerState uDepthSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    int uUseShadowFactor;
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

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uDepthSampler, uv).r;
    if (IsBackgroundDepth(depth))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // RT0 already includes unshadowed fill lights + emissive (split lighting pass).
    const float3 fillAndEmissive = uDirectLighting.Sample(uDirectLightingSampler, uv).rgb;
    const float4 sunShadow = uSunShadowMap.Sample(uSunShadowSampler, uv);
    const float shadowFactor = uUseShadowFactor != 0 ? sunShadow.a : 1.0;
    const float3 sunDirect = sunShadow.rgb * shadowFactor;

    return float4(max(fillAndEmissive + sunDirect, 0.0.xxx), 1.0);
}
