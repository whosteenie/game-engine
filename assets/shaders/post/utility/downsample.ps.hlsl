Texture2D uInput : register(t0);

SamplerState uInputSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return uInput.Sample(uInputSampler, input.texCoord);
}
