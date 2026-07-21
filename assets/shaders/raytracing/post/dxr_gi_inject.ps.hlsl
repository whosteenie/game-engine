// DXR Phase D9 — RT diffuse GI composite (devdoc/dxr/diffuse-gi.md).
// REPLACES the SH diffuse ambient in RT1 with one-bounce ray-traced diffuse GI. The PBR raster
// omits the SH diffuse ambient from RT1 when GI is active (uOmitDiffuseIbl), so this pass supplies
// the diffuse indirect: the cosine-hemisphere trace already integrates the sky (miss -> env) PLUS
// one bounce PLUS true local occlusion, so it supersedes the crude 9-coefficient SH sky rather
// than stacking on top of it.
//
//   indirect_out = RT1_noDiffuseAmbient + albedo * diffuseEnergy * giRadiance * uStrength
//
// diffuseEnergy = (1 - F) * (1 - metallic) matches the raster's Cook-Torrance diffuse weight
// (pbr.ps.hlsl): metals have no diffuse lobe, and the (1 - F) term conserves energy vs the
// specular lobe (without it GI reads too hot). When there is no fresh GI trace (warmup/failure,
// uHasGiTrace = 0) the raster still omitted the ambient, so we restore a recomputed SH ambient as
// a transient fallback. Mutually exclusive with SSGI inject (SceneRenderer gates one at a time).

#include "../../common/screen_space_common.hlsl"

Texture2D uIndirectMap : register(t0);   // current indirect chain (RT1 -> spec composite -> here)
Texture2D uGiDenoisedMap : register(t1); // NRD RELAX_DIFFUSE output (or raw when denoise off)
Texture2D uDepthMap : register(t2);
Texture2D uMaterial0Map : register(t3);  // albedo.rgb + roughness.a (RT5)
Texture2D uMaterial1Map : register(t4);  // metallic.r + emissive.gba (RT6)
Texture2D uNormalMap : register(t5);     // shading normal (RT2)

// s0: linear clamp (colors, GI radiance). s1: point clamp (G-buffer).
SamplerState uLinearSampler : register(s0);
SamplerState uPointSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvView;
    float2 uGiUvScale;    // dispatch/texture region of the GI buffers (see dxr-reflections.md)
    float uStrength;
    int uDebugGiInject;   // 1 = visualize the injected GI delta
    int uHasGiTrace;      // 1 = fresh GI trace this frame; 0 = restore SH ambient (transient)
    float uEnvironmentIntensity;
    float2 _pad0;
    float4 uIrradianceSh[9]; // L2 SH diffuse irradiance (fallback only)
};

static const float PI = 3.14159265;

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Direction from the surface toward the camera (V in BRDF convention).
float3 ViewDirectionWorld(float2 texCoord)
{
    const float2 clipXY = DepthUvToClipXY(texCoord);
    const float4 viewFar = mul(uInvProjection, float4(clipXY, 1.0, 1.0));
    const float3 towardScene = normalize(viewFar.xyz / viewFar.w);
    return -normalize(mul((float3x3)uInvView, towardScene));
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    const float3 maxReflection = max(1.0.xxx - roughness, f0);
    return f0 + (maxReflection - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Matches EvaluateDiffuseIrradianceSh in pbr.ps.hlsl / hit_shading.hlsli.
float3 EvaluateDiffuseIrradianceSh(float3 normal)
{
    const float3 n = normalize(normal);
    const float x = n.x;
    const float y = n.y;
    const float z = n.z;

    float3 irradiance = uIrradianceSh[0].rgb * 0.282095;
    irradiance += uIrradianceSh[1].rgb * (0.488603 * y);
    irradiance += uIrradianceSh[2].rgb * (0.488603 * z);
    irradiance += uIrradianceSh[3].rgb * (0.488603 * x);
    irradiance += uIrradianceSh[4].rgb * (1.092548 * x * y);
    irradiance += uIrradianceSh[5].rgb * (1.092548 * y * z);
    irradiance += uIrradianceSh[6].rgb * (0.315392 * (3.0 * z * z - 1.0));
    irradiance += uIrradianceSh[7].rgb * (1.092548 * z * x);
    irradiance += uIrradianceSh[8].rgb * (0.546274 * (x * x - y * y));
    return max(irradiance, 0.0.xxx);
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uPointSampler, uv).r;
    const float3 indirect = uIndirectMap.Sample(uLinearSampler, uv).rgb;

    // Background: RT1 carries the sky — pass it through unchanged (zeroing it black-outlines
    // grid lines drawn over the skybox, RTQ-05).
    if (depth >= 0.9999)
    {
        return float4(indirect, 1.0);
    }

    const float4 material0 = uMaterial0Map.Sample(uPointSampler, uv);
    const float3 albedo = material0.rgb;
    const float roughness = material0.a;
    const float metallic = uMaterial1Map.Sample(uPointSampler, uv).r;
    const float3 normal = uNormalMap.Sample(uPointSampler, uv).rgb;

    // Energy-conserving diffuse weight = (1 - F) * (1 - metallic), matching the raster's diffuse
    // IBL term (pbr.ps.hlsl:804-806). Metals get ~0 (their indirect is the specular reflection).
    const float3 viewDir = ViewDirectionWorld(uv);
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float nDotV = max(dot(normalize(normal), viewDir), 0.0);
    const float3 specularEnergy = FresnelSchlickRoughness(nDotV, f0, max(roughness, 0.55));
    const float3 diffuseWeight = albedo * (1.0.xxx - specularEnergy) * (1.0 - metallic);

    float3 injected;
    if (uHasGiTrace != 0)
    {
        // GI buffers only cover the top-left dispatch region of their allocation.
        const float2 giUv = uv * uGiUvScale;
        const float3 giRadiance = uGiDenoisedMap.Sample(uLinearSampler, giUv).rgb;
        injected = diffuseWeight * giRadiance * uStrength;
    }
    else
    {
        // Transient fallback: the raster omitted the SH ambient but there is no GI yet. Restore a
        // recomputed SH diffuse ambient so surfaces don't flash dark during warmup.
        injected = diffuseWeight * EvaluateDiffuseIrradianceSh(normal) * uEnvironmentIntensity / PI;
    }

    if (uDebugGiInject != 0)
    {
        return float4(max(injected, 0.0.xxx), 1.0);
    }

    return float4(max(indirect + injected, 0.0.xxx), 1.0);
}
