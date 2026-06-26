cbuffer PerVertex : register(b0)
{
    float4x4 uProjection;
    float4x4 uView;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.localPos = input.position;

    float4x4 view = uView;
    view[3] = float4(0.0, 0.0, 0.0, 1.0);

    float4 clipPos = mul(uProjection, mul(view, float4(input.position, 1.0)));
    output.position = clipPos.xyww;
    return output;
}
