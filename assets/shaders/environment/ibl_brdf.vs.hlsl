struct VSInput
{
    float2 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.texCoord = input.texCoord;
    output.position = float4(input.position, 0.0, 1.0);
    return output;
}
