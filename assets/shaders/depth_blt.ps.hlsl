Texture2D uDepth : register(t0);

SamplerState uDepthSampler : register(s0)
{
    Filter = MIN_MAG_MIP_POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

void main(PSInput input, out float outDepth : SV_Depth)
{
    outDepth = uDepth.Sample(uDepthSampler, input.texCoord).r;
}
