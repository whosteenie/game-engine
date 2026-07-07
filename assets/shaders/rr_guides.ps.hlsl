// DLSS Ray Reconstruction guide generation (devdoc/dxr-dlss-rr.md, Phases RR1/RR4).
// Produces the material guides RR consumes, at render (internal) resolution, from the G-buffer.
// One shader, selected by uGuideMode; the renderer draws it once per guide target.
//   0 = diffuse albedo   (SL kBufferTypeAlbedo)            = albedo * (1 - metallic)
//   1 = specular albedo  (SL kBufferTypeSpecularAlbedo)    = F0 = lerp(0.04, albedo, metallic)
//   2 = normal-roughness (SL kBufferTypeNormalRoughness, PACKED) = world normal in rgb, roughness in a
//   3 = spec hit distance(SL kBufferTypeSpecularHitDistance)     = reflection ray length, world units
//
// Encoding contract (RR4 — VALIDATED against sl_dlss_d.h + sl_core_types.h):
//  - Normal-roughness uses DLSSDNormalRoughnessMode::ePacked: raw signed world-space normal in rgb,
//    linear roughness in .a, in an fp16 (signed) target. RR transforms world->view itself via the
//    DLSSDOptions worldToCameraView/cameraViewToWorld we supply, so world-space (not view-space) is
//    correct and no N*0.5+0.5 remap is needed. This is the single place to adjust if that ever changes.
//  - Spec hit distance is the raw reflection ray length in world units (miss = max trace distance),
//    exactly as the reflection trace packs it into radiance.a — RR uses it to reproject/sharpen
//    reflections. Written to a single-channel (R16_FLOAT) target so channel choice is unambiguous.

Texture2D uNormalMap : register(t0);    // RT2 shading normal (world space)
Texture2D uMaterial0Map : register(t1); // albedo.rgb + roughness.a (RT5)
Texture2D uMaterial1Map : register(t2); // metallic.r + emissive.gba (RT6)
Texture2D uReflectionRaw : register(t3); // raw RT reflection radiance.rgb + hit distance.a (mode 3)

SamplerState uPointSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uGuideMode; // 0 = diffuse albedo, 1 = specular albedo, 2 = normal-roughness, 3 = spec hit dist
    float uReflectionUvScaleX; // render-res UV -> reflection-buffer UV (reflection may be quality-scaled)
    float uReflectionUvScaleY;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float4 material0 = uMaterial0Map.Sample(uPointSampler, uv);
    const float3 albedo = material0.rgb;
    const float roughness = material0.a;
    const float metallic = uMaterial1Map.Sample(uPointSampler, uv).r;

    if (uGuideMode == 0)
    {
        return float4(albedo * (1.0 - metallic), 1.0); // diffuse albedo
    }
    if (uGuideMode == 1)
    {
        return float4(lerp(0.04.xxx, albedo, metallic), 1.0); // specular albedo (F0)
    }
    if (uGuideMode == 3)
    {
        // Specular hit distance: raw reflection ray length (world units) from the reflection trace,
        // stored in radiance.a. The reflection buffer may run quality-scaled, so remap the UV.
        const float2 reflUv = uv * float2(uReflectionUvScaleX, uReflectionUvScaleY);
        const float hitDistance = uReflectionRaw.Sample(uPointSampler, reflUv).a;
        return float4(hitDistance, 0.0, 0.0, 0.0); // single-channel (R16_FLOAT) target keeps .r
    }

    // Packed normal-roughness: world normal in rgb (raw signed), roughness in a.
    const float3 normal = normalize(uNormalMap.Sample(uPointSampler, uv).rgb);
    return float4(normal, roughness);
}
