cbuffer PerVertex : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(uProjection, mul(uView, float4(input.position, 1.0)));
    return output;
}
