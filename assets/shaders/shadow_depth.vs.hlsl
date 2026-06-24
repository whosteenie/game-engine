cbuffer PerVertex : register(b0)
{
    float4x4 uModel;
    float4x4 uLightSpaceMatrix;
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
    output.position = mul(uLightSpaceMatrix, mul(uModel, float4(input.position, 1.0)));
    return output;
}
