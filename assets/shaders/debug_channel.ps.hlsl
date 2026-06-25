Texture2D uInput : register(t0);
SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uOutputRgb;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float4 sampled = uInput.Sample(uInputSampler, input.texCoord);
    if (uOutputRgb != 0)
    {
        return float4(sampled.rgb, 1.0);
    }

    return float4(sampled.r, sampled.r, sampled.r, 1.0);
}
