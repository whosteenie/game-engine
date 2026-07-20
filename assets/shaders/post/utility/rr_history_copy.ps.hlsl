Texture2D<float4> uSource : register(t0);
SamplerState uPointSampler : register(s0);

cbuffer PerFrame : register(b0)
{
    float uDepthOnly;
    float3 _padding;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float4 value = uSource.SampleLevel(uPointSampler, input.texCoord, 0);
    // Preserve precision for nonlinear depth near 1 when stored in R16F.
    return uDepthOnly > 0.5 ? (1.0 - value.r).rrrr : value;
}
