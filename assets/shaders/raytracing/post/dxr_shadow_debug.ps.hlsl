// DXR Phase D8 shadow debug view (devdoc/dxr/shadows.md).
// Raw mode: the 1-spp packed penumbra buffer (R16F). Lit pixels pack NRD_FP16_MAX (65504);
//   occluded pixels pack a penumbra radius in [0; 32768]. Map to a binary lit/shadowed mask so
//   the aliased 1-spp shadow edges are visible (this is the noisy signal SIGMA consumes).
// Denoised mode: OUT_SHADOW_TRANSLUCENCY (R16F) stores sqrt(shadow); square it to recover the
//   [0; 1] shadow term (SIGMA_BackEnd_UnpackShadow).
Texture2D uInput : register(t0);
SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uRawPenumbra;  // 1 = raw penumbra buffer, 0 = denoised shadow buffer
    float2 uUvScale;   // valid UV region of a kept-alive larger allocation (<= 0 => full)
    float _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 scale = uUvScale.x <= 0.0 ? float2(1.0, 1.0) : uUvScale;
    const float sampled = uInput.Sample(uInputSampler, input.texCoord * scale).r;

    float shadow;
    if (uRawPenumbra != 0)
    {
        // Lit sentinel is NRD_FP16_MAX (65504); anything below is an occluder → shadowed.
        shadow = sampled >= 40000.0 ? 1.0 : 0.0;
    }
    else
    {
        shadow = saturate(sampled * sampled);
    }

    return float4(shadow, shadow, shadow, 1.0);
}
