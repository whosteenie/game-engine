Texture2D uEdge : register(t0);
SamplerState uEdgeSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float3 uColor;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float sharp = uEdge.Sample(uEdgeSampler, input.texCoord).r;
    clip(sharp - 0.004);

    return float4(uColor, sharp);
}
