// DLSS Ray Reconstruction guide generation (devdoc/dxr-dlss-rr.md, Phase RR1).
// Produces the material guides RR consumes, at render (internal) resolution, from the G-buffer.
// One shader, selected by uGuideMode; the renderer draws it once per guide target.
//   0 = diffuse albedo   (SL kBufferTypeAlbedo)         = albedo * (1 - metallic)
//   1 = specular albedo  (SL kBufferTypeSpecularAlbedo) = F0 = lerp(0.04, albedo, metallic)
//   2 = normal-roughness (SL kBufferTypeNormalRoughness, PACKED) = world normal in rgb, roughness in a
//
// Encoding note (RR4 will validate against the SL RR guide): the packed normal is written as raw
// world-space [-1,1] into an fp16 target (a signed format), with roughness in .a. If RR expects
// N*0.5+0.5 or octahedral, that is a one-line change here — this pass is the single place to adjust.

Texture2D uNormalMap : register(t0);    // RT2 shading normal (world space)
Texture2D uMaterial0Map : register(t1); // albedo.rgb + roughness.a (RT5)
Texture2D uMaterial1Map : register(t2); // metallic.r + emissive.gba (RT6)

SamplerState uPointSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uGuideMode; // 0 = diffuse albedo, 1 = specular albedo, 2 = normal-roughness
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

    // Packed normal-roughness: world normal in rgb (raw signed), roughness in a.
    const float3 normal = normalize(uNormalMap.Sample(uPointSampler, uv).rgb);
    return float4(normal, roughness);
}
