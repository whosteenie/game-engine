struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

cbuffer PerFrame : register(b0)
{
    float2 uMotionValue;
    float2 _padding;
};

float2 main(PSInput input) : SV_Target
{
    return uMotionValue;
}
