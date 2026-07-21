Texture2D uGlow : register(t0);
Texture2D uEdge : register(t1);

SamplerState uGlowSampler : register(s0);
SamplerState uEdgeSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float3 uColor;
    float uGlowIntensity;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float blurred = uGlow.Sample(uGlowSampler, input.texCoord).r;
    float sharp = uEdge.Sample(uEdgeSampler, input.texCoord).r;

    float halo = max(blurred - sharp * 0.92, 0.0);
    float strength = pow(halo, 2.2) * uGlowIntensity;
    clip(strength - 0.004);

    return float4(uColor * strength, 1.0);
}
