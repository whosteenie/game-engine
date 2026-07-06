// DXR Phase D6 — RT specular composite (devdoc/dxr-groundwork.md Phase D6).
// Standalone RT pass, mutually exclusive with ssr_indirect.ps.hlsl (same energy pattern,
// no SSR buffer reads): indirect_out = RT1 - spec_ibl + lerp(spec_ibl, rt * envBrdf, w).
// w comes from the RAW trace's hit distance (denoised alpha carries NRD history length,
// not hit data): miss/far rays keep the recomputed IBL term.

#include "screen_space_common.hlsl"

Texture2D uIndirectMap : register(t0);   // RT1
Texture2D uRtDenoisedMap : register(t1); // NRD output (or raw when denoise off)
Texture2D uRtRawMap : register(t2);      // raw trace: .a = hit distance
Texture2D uDepthMap : register(t3);
Texture2D uNormalMap : register(t4);     // shading normal (RT2)
Texture2D uMaterial0Map : register(t5);  // albedo + roughness
Texture2D uMaterial1Map : register(t6);  // metallic + emissive
TextureCube uPrefilterMap : register(t7);
Texture2D uBrdfLut : register(t8);

// s0: linear clamp (colors, prefiltered env, BRDF LUT). s1: point clamp (G-buffer, raw).
SamplerState uLinearSampler : register(s0);
SamplerState uPointSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvView;
    float uEnvironmentIntensity;
    float uMaxReflectionLod;
    float uStrength;
    float uMaxTraceDistance;
    float2 uRtUvScale; // dispatch/texture region of the RT buffers (see dxr-reflections.md)
    int uDebugSpecReplacement;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Returns the direction from the surface toward the camera (V in BRDF convention).
float3 ViewDirectionWorld(float2 texCoord)
{
    const float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewFar = mul(uInvProjection, float4(clipXY, 1.0, 1.0));
    const float3 towardScene = normalize(viewFar.xyz / viewFar.w);
    return -normalize(mul((float3x3)uInvView, towardScene));
}

float3 EnvironmentBrdf(float3 f0, float nDotV, float roughness)
{
    const float2 envBrdf = uBrdfLut.Sample(uLinearSampler, float2(nDotV, roughness)).rg;
    return f0 * envBrdf.x + envBrdf.y;
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uPointSampler, uv).r;
    if (depth >= 0.9999)
    {
        // Background: RT1 carries the SKY (sky_background.ps writes oIndirect, and the
        // screen composite's grid-over-sky branch reads it). Pass it through unchanged —
        // returning 0 here black-outlined every grid line drawn over the skybox.
        return float4(uIndirectMap.Sample(uLinearSampler, uv).rgb, 1.0);
    }

    const float3 indirect = uIndirectMap.Sample(uLinearSampler, uv).rgb;
    const float3 worldNormal = normalize(uNormalMap.Sample(uPointSampler, uv).rgb);
    const float4 material0 = uMaterial0Map.Sample(uPointSampler, uv);
    const float metallic = uMaterial1Map.Sample(uPointSampler, uv).r;
    const float3 albedo = material0.rgb;
    const float roughness = material0.a;

    const float3 viewDir = ViewDirectionWorld(uv);
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float nDotV = max(dot(worldNormal, viewDir), 0.0);
    const float3 environmentBrdf = EnvironmentBrdf(f0, nDotV, roughness);

    const float3 reflection = reflect(-viewDir, worldNormal);
    const float3 prefilteredColor = uPrefilterMap.SampleLevel(
        uLinearSampler,
        reflection,
        roughness * uMaxReflectionLod).rgb;
    const float3 specIbl = prefilteredColor * environmentBrdf * uEnvironmentIntensity;

    // RT buffers only cover the top-left dispatch region of their allocation.
    const float2 rtUv = uv * uRtUvScale;
    const float3 rtRadiance = uRtDenoisedMap.Sample(uLinearSampler, rtUv).rgb;
    const float hitDistance = uRtRawMap.Sample(uPointSampler, rtUv).a;

    // Hit validity: rays that reached maxTraceDistance are misses (the trace stored the
    // prefiltered env in rgb, but the composite's own BRDF-weighted IBL term is the
    // energy-correct fallback). Fade out slightly before the cap to avoid a hard seam.
    const float hitMask =
        1.0 - smoothstep(uMaxTraceDistance * 0.85, uMaxTraceDistance * 0.99, hitDistance);

    // Receiver-distance fade: distant/grazing receivers mostly produce degenerate self-hits
    // (dark reflected floor, compressed grid moire at the horizon) — hand those back to IBL.
    const float2 fadeClipXY = DepthUvToClipXY(uv);
    const float4 viewH = mul(uInvProjection, float4(fadeClipXY, depth, 1.0));
    const float receiverViewZ = viewH.z / viewH.w;
    const float receiverFade =
        1.0 - smoothstep(uMaxTraceDistance * 0.5, uMaxTraceDistance, receiverViewZ);

    const float replacementWeight = saturate(hitMask * uStrength * receiverFade);

    if (uDebugSpecReplacement != 0)
    {
        return float4(replacementWeight.xxx, 1.0);
    }

    // Same split-sum weighting as the IBL term it replaces (mirrors SSR-04's fix).
    const float3 rtSpecular = rtRadiance * environmentBrdf;
    const float3 specFinal = lerp(specIbl, rtSpecular, replacementWeight);
    return float4(max(indirect - specIbl + specFinal, 0.0.xxx), 1.0);
}
