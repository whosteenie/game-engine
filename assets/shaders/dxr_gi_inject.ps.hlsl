// DXR Phase D9 — RT diffuse GI composite (devdoc/dxr-diffuse-gi.md).
// Adds one-bounce ray-traced diffuse GI into RT1 (indirect). Mirrors dxr_indirect.ps.hlsl's
// binding/uv-scale/background contract, but the diffuse response is simply albedo * radiance:
// the cosine pdf already folded the cosine into the trace average, and the 1/pi (Lambertian
// BRDF) and pi (pdf) cancel — so no extra factors here.
//
//   indirect_out = RT1 + albedo * giRadiance * uStrength
//
// Additive-with-strength (same policy as SSGI inject). RT1 already carries the SH-ambient
// diffuse term, so this reduces to an ambient boost at v1 — lower Environment Intensity if the
// scene washes out. Mutually exclusive with SSGI inject (SceneRenderer gates one at a time).

Texture2D uIndirectMap : register(t0);   // RT1 indirect (also carries the sky at background)
Texture2D uGiDenoisedMap : register(t1); // NRD RELAX_DIFFUSE output (or raw when denoise off)
Texture2D uDepthMap : register(t2);
Texture2D uMaterial0Map : register(t3);  // albedo.rgb + roughness.a (RT5)

// s0: linear clamp (colors, GI radiance). s1: point clamp (G-buffer).
SamplerState uLinearSampler : register(s0);
SamplerState uPointSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float2 uGiUvScale; // dispatch/texture region of the GI buffers (see dxr-reflections.md)
    float uStrength;
    int uDebugGiInject; // 1 = visualize the injected delta (albedo * gi * strength)
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

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

    const float3 albedo = uMaterial0Map.Sample(uPointSampler, uv).rgb;

    // GI buffers only cover the top-left dispatch region of their allocation.
    const float2 giUv = uv * uGiUvScale;
    const float3 giRadiance = uGiDenoisedMap.Sample(uLinearSampler, giUv).rgb;

    const float3 injected = albedo * giRadiance * uStrength;

    if (uDebugGiInject != 0)
    {
        return float4(max(injected, 0.0.xxx), 1.0);
    }

    return float4(max(indirect + injected, 0.0.xxx), 1.0);
}
