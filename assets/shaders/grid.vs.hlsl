cbuffer PerVertex : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
    float2 uGridSnapOrigin;
    float uGridHeight;
    float _pad0;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.worldPos = float3(
        input.position.x + uGridSnapOrigin.x,
        uGridHeight,
        input.position.z + uGridSnapOrigin.y);
    output.position = mul(uProjection, mul(uView, float4(output.worldPos, 1.0)));
    return output;
}
